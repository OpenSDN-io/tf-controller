# OpenSDN Virtual Network Controller

## License

This software is licensed under the Apache License, Version 2.0 (the "License");
you may not use this software except in compliance with the License.
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

## Overview

The OpenSDN (Tungsten Fabric formerly) Virtual Network Controller repository contains the code for the configuration management, analytics and control-plane components of the OpenSDN network virtualization solution.

* The data-plane component (aka vrouter) is available in a separate code repository (http://github.com/OpenSDN-io/tf-vrouter).

* The configuration management component is located under `src/config`. It provides a REST API to an orchestration system and translates the system configuration as an IF-MAP database.
 
* The configuration schema is used by the OpenSDN controller and is defined under `src/schema`. A code generation tool is used to convert the schema into accessor methods used by the API clients (`src/api-lib`), the API server as well as the control-plane components.

* The control-node daemon code is located under `src/{bgp,control-node,ifmap,xmpp}`. It implements the operational state database and interoperates with networking equipment as well as the compute-node agents. The protocol used between the control-node and the compute-node agents is documented as an [IETF draft](http://tools.ietf.org/html/draft-ietf-l3vpn-end-system-01). This component contains the network reachability (a.k.a. routing) information in the system which is transient and can potentially have a higher rate of change than the configuration state.

* The compute-node agent (`src/vnsw`) is a deamon than runs on every
  compute node and programs the data-plane in the host operating system.

* Data gathered from all these components is collected into a logically centralized database (`src/{analytics,opserver}`).

* The source code of the aforementioned components is being documented using doxygen to produce online manual [https://opensdn-io.github.io/doxygen-docs/index.html](https://opensdn-io.github.io/doxygen-docs/index.html).

## Contributing code

* For technical questions about contributions, as well as to communicate developers and community, visit [https://docs.opensdn.io/contributing-to-opensdn/getting-started/getting-started-with-opensdn-development.html](https://docs.opensdn.io/contributing-to-opensdn/getting-started/getting-started-with-opensdn-development.html)

## Coding style

The OpenSDN source code includes modules writen in C, C++ and Python; there
is also a Java API library, Lua scripts in analytics IDL, and a code generator
based on a mini-DSL. Each of these languages has a distinct coding style.

### C++

The C++ code should follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
with the next main distinctions:
    * the 4-space indentation is used rather than 2-space;
    * for declaration comments (auto-documenting)
     [doxygen conventions](https://www.doxygen.nl/manual/docblocks.html)
     are used.

C++ code submissions require a unit-test for the class
interface; more complex code changes require tests for the
functionality across multiple modules.

Bugs should be first reproduced in a unit-test and then resolved.

### Python

Python code follows PEP-8 style.