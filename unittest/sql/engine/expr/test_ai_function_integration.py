"""Exercise AI SQL against a deterministic local mock; no external model calls.

Run against a disposable instance: python3 test_ai_function_integration.py --port 2888
"""
import argparse
import hashlib
import http.server
import json
import subprocess
import threading
import uuid


def quote(value):
    return "'" + value.replace('\\', '\\\\').replace("'", "''") + "'"


class Handler(http.server.BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.append((self.path, data))
        if self.path.endswith('/generate'):
            assert data['stream'] is False
            response = {'response': data['prompt']}
        elif self.path.endswith('/chat/completions'):
            response = {'choices': [{'message': {'content': data['messages'][-1]['content']}}]}
        elif self.path.endswith('/rerank'):
            response = {'results': list(reversed([
                {'index': i, 'relevance_score': float(text.count(data['query']))}
                for i, text in enumerate(data['documents'])]))}
        else:
            inputs = data['input']
            if isinstance(inputs, str):
                inputs = [inputs]
            dimension = data.get('dimensions', 768)
            vectors = [[int(hashlib.sha256(text.encode()).hexdigest()[:6], 16)] * dimension
                       for text in inputs]
            if self.path.startswith('/invalid/'):
                vectors[0][0] = True
            if self.path.endswith('/embed'):
                response = {'embeddings': vectors}
            else:
                response = {'data': list(reversed([
                    {'index': i, 'embedding': vector} for i, vector in enumerate(vectors)]))}
        payload = json.dumps(response).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--mysql', default='mysql')
    args = parser.parse_args()
    prefix = 'ai_review_' + uuid.uuid4().hex[:8]
    models = []
    endpoints = []
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    def sql(statement, error=False):
        result = subprocess.run([args.mysql, '-h127.0.0.1', '-P' + str(args.port),
                                 '-uroot', '--batch', '--raw', '--skip-column-names',
                                 '--default-character-set=utf8mb4'],
                                input=statement, text=True, capture_output=True, timeout=35)
        if error:
            assert result.returncode != 0, statement
        else:
            assert result.returncode == 0, result.stderr + '\n' + statement
        return result.stdout.strip().splitlines()

    def create(suffix, kind, provider, path):
        name = prefix + suffix
        config = {'type': kind, 'model_name': 'mock-model'}
        sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s);' % (quote(name), quote(json.dumps(config))))
        models.append(name)
        config = {'ai_model_name': name, 'scope': 'all', 'provider': provider,
                  'url': 'http://127.0.0.1:%d%s' % (server.server_port, path),
                  'access_key': '' if provider.lower() == 'ollama' else 'mock-key'}
        sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s);' %
            (quote(name + '_ep'), quote(json.dumps(config))))
        endpoints.append(name + '_ep')
        return quote(name)

    try:
        print('Checking model-not-found cleanup...', flush=True)
        sql('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s);' % quote(prefix), error=True)
        complete = create('_c', 'completion', 'openai', '/chat/completions')
        embed = create('_e', 'dense_embedding', 'openai', '/embeddings')
        rerank = create('_r', 'rerank', 'siliconflow', '/rerank')
        ollama = create('_o', 'completion', 'Ollama', '/api/generate')
        ollama_embed = create('_oe', 'dense_embedding', 'OLLAMA', '/api/embed')
        bad = create('_bad', 'dense_embedding', 'openai', '/invalid/embeddings')
        print('Checking completion, templates, config, and Ollama...', flush=True)
        assert sql("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'hello',NULL));" % complete) == ['hello']
        assert sql("SELECT LENGTH(JSON_UNQUOTE(AI_COMPLETE(%s,REPEAT('x',70000))));" % complete) == ['70000']
        assert sql("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,AI_PROMPT('{0}/{0}/{1}','outer',AI_PROMPT('inner {0}','x')),JSON_OBJECT('temperature',0.2,'max_tokens',16)));" % complete) == ['outer/outer/inner x']
        assert sql("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'hello'));" % ollama) == ['hello']
        sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,JSON_OBJECT('provider','oLlAmA'));" % quote(prefix + '_o_ep'))
        assert sql("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'after alter'));" % ollama) == ['after alter']
        sql("SELECT AI_COMPLETE(%s,AI_PROMPT('{99}','x'));" % complete, error=True)
        print('Checking embedding dimensions and malformed responses...', flush=True)
        assert sql("SELECT JSON_LENGTH(AI_EMBED(%s,'x'));" % embed) == ['768']
        assert sql("SELECT JSON_LENGTH(AI_EMBED(%s,'x',3));" % embed) == ['3']
        assert sql("SELECT JSON_LENGTH(AI_EMBED(%s,'x',3));" % ollama_embed) == ['3']
        for dimension in ['0', '-1', '1.5', 'NULL']:
            sql("SELECT AI_EMBED(%s,'x',%s);" % (embed, dimension), error=True)
        sql("SELECT AI_EMBED(%s,'x',3);" % bad, error=True)
        print('Checking 135 rows, NULL, filtering, LIMIT, and response index mapping...', flush=True)
        sql('CREATE DATABASE ' + prefix)
        values = ','.join('(%d,%s)' % (i, 'NULL' if i % 11 == 0 else quote('row%d' % i)) for i in range(135))
        sql('CREATE TABLE %s.t(id INT PRIMARY KEY, content TEXT); INSERT INTO %s.t VALUES %s;' % (prefix, prefix, values))
        Handler.requests.clear()
        result = sql('SELECT id,AI_EMBED(%s,content,3) FROM %s.t ORDER BY id;' % (embed, prefix))
        assert len(result) == 135
        for line in result:
            index, vector = line.split('\t')
            index = int(index)
            if index % 11 == 0:
                assert vector == 'NULL'
            else:
                expected = int(hashlib.sha256(('row%d' % index).encode()).hexdigest()[:6], 16)
                assert json.loads(vector) == [expected] * 3, line
        counts = [len(body['input']) for path, body in Handler.requests if path == '/embeddings']
        assert sum(counts) == 122 and max(counts) > 1 and len(counts) > 1, counts
        result = sql('SELECT id,JSON_LENGTH(AI_EMBED(%s,content,3)) FROM %s.t WHERE id>125 ORDER BY id LIMIT 4;' % (embed, prefix))
        assert result == ['126\t3', '127\t3', '128\t3', '129\t3'], result
        result = sql('SELECT id,JSON_LENGTH(AI_EMBED(%s,content,1+id%%3)) FROM %s.t WHERE id<15 ORDER BY id;' % (embed, prefix))
        assert result == ['%d\t%s' % (i, 'NULL' if i % 11 == 0 else str(1 + i % 3)) for i in range(15)], result
        print('Checking rerank batching, global mapping, and stable ties...', flush=True)
        documents = [{'id': str(i), 'text': 'apple' if i % 2 else 'banana'} for i in range(43)]
        result = sql("SELECT AI_RERANK(%s,'apple',%s,'text');" % (rerank, quote(json.dumps(documents))))
        ids = [int(item['id']) for item in json.loads(result[0])]
        assert ids == list(range(1, 43, 2)) + list(range(0, 43, 2)), ids
        print('PASS: AI SQL integration; embedding request sizes: ' + str(counts), flush=True)
    finally:
        for name in endpoints:
            sql('CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s);' % quote(name))
        for name in models:
            sql('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s);' % quote(name))
        sql('DROP DATABASE IF EXISTS ' + prefix)
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    main()
