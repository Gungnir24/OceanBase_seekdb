/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


 #define USING_LOG_PREFIX SQL_SESSION

#include "share/schema/ob_schema_struct.h"
#include "ob_ai_model_priv_util.h"

namespace oceanbase
{
namespace sql
{
ObAIServiceEndpointPrivUtil::ObAIServiceEndpointPrivUtil(share::schema::ObSchemaGetterGuard &schema_guard)
  : schema_guard_(schema_guard)
{
}

int ObAIServiceEndpointPrivUtil::check_ai_model_priv(ObIAllocator &allocator,
                                    const share::schema::ObSessionPrivInfo &session_priv,
                                    ObPrivSet ai_priv_type,
                                    bool &has_priv)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  has_priv = false;
  // AI model privileges are global (user level) privileges.  The session
  // privilege info is resolved from the user catalog, so the privilege bits of
  // the invoking user decide the outcome.  Role granted privileges are folded
  // into the same bit set.
  if (OB_PRIV_HAS_ANY(session_priv.user_priv_set_, ai_priv_type)
      || OB_PRIV_HAS_ANY(session_priv.user_priv_set_, OB_PRIV_SUPER)) {
    has_priv = true;
  }
  return ret;
}

int ObAIServiceEndpointPrivUtil::check_create_ai_model_priv(
    ObIAllocator &allocator,
    const share::schema::ObSessionPrivInfo &session_priv,
    bool &has_priv)
{
  return check_ai_model_priv(allocator, session_priv, 
                                       AI_PRIV_CREATE, has_priv);
}

int ObAIServiceEndpointPrivUtil::check_alter_ai_model_priv(
    ObIAllocator &allocator,
    const share::schema::ObSessionPrivInfo &session_priv,
    bool &has_priv)
{
  return check_ai_model_priv(allocator, session_priv, 
                                       AI_PRIV_ALTER, has_priv);
}

int ObAIServiceEndpointPrivUtil::check_drop_ai_model_priv(
    ObIAllocator &allocator,
    const share::schema::ObSessionPrivInfo &session_priv,
    bool &has_priv)
{
  return check_ai_model_priv(allocator, session_priv, 
                                       AI_PRIV_DROP, has_priv);
}

int ObAIServiceEndpointPrivUtil::check_access_ai_model_priv(
    ObIAllocator &allocator,
    const share::schema::ObSessionPrivInfo &session_priv,
    bool &has_priv)
{
  return check_ai_model_priv(allocator, session_priv, 
                                       AI_PRIV_ACCESS, has_priv);
}

}
}
