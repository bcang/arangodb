////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_HYDRA_CHANNEL_BASE_H
#define ARANGODB_HYDRA_CHANNEL_BASE_H 1

#include <velocypack/velocypack-aliases.h>
#include <algorithm>
#include "Basics/Common.h"
#include "RestHandler/RestVocbaseBaseHandler.h"

namespace arangodb {
namespace hydra {

class RestHydraHandler : public arangodb::RestVocbaseBaseHandler {
  public:
    explicit RestHydraHandler(GeneralRequest*, GeneralResponse*);
    
  public:
    bool isDirect() const override { return false; }
    RestStatus execute() override;
    char const* name() const override {return "HydraHandler";}
  
  private:
  
   void handleJobControl();
   void handleNotify();
   void handleMailbox();
};
}
}
#endif
