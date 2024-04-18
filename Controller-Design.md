Border Gateway Protocol (BGP)
=============================

Overview
--------

The OpenSDN BGP implementation was designed from scratch to run on modern server environemnts. The main goals where to be able to take advantage of multicore CPUs, large (>4G) memory footprints and modern software development techniques. OpenSDN's initial target application (network virtualization) does not in itself require an implementation to support a high rate of routing updates or a large number of routes but the engineering team felt that given the opportunity to write an implementation from scratch we wanted to be able to tackle the problem of concurrency from the beginning.

Concurrency
-----------

The BGP protocol can scale horizontally by adding BGP speakers which are then federated as a Route Reflector mesh. That approach is very effective when scaling up in one of the parallelizable dimensions: number of peers; less so when it come to scaling in the order dimension: number of routes. While it is possible to implement a load-balancer like mechanism that would proxy the BGP session of a peer and distribute the route load accross multiple route reflection meshes the engineering team felt that this approach would expose too much complexity in situations where the scalling requirements are not particularly demanding.

As a result we adopted the more traditional approach of designing a multi-threaded implementation in a single process that had the goal of parallelizing both peer processing as well as route processing. In BGP routes with different prefixes can be processed independently is most circustances, except when implementing functionality such as route aggregation or next-hop resolution.

Having decided to use multi-threading we also knew we needed to find an alternative to the use of mutexes as the primary mechanism to express data concurrency policies. Without a design that put a framework around concurrency and localizes locking to specific modules of the code multithreading can become a very significant burden on feature development and code stability.

The concurrency design we ended up selecting is based on two concepts:

- pipeline stage;
- dataset.

In a CPU design, a pipeline stage is independent from the stages that preceed and follow it, except for a well defined set of registers for which there is a locking mechanism in place. An example of dataset parallelism if an NPU that processes packets in parallel; or an hadoop job.

The implementation does not capture these concepts literally but achieves what we consider to be a reasonable approximation. The [Task class](https://github.com/OpenSDN-io/tf-common/blob/master/base/task.h) (or see the [docs index](https://opensdn-io.github.io/doxygen-docs/html/classes.html#letter_T)) allows the programmer to specificy that a particular callback should executed when determined by a given task_id and task_instance parameters. These correspond roughly to the concepts of pipeline stage and dataset.

The application can then specificy scheduling policies using these parameters. As an example, processing configuration changes is mutually exclusive with any other Task; code that runs under the bgp::Config task can modify any data-structure without explicitly acquiring a mutex. The bulk of BGP processing however is performed by tasks with a specific task_instance identifier that operate on a subset of the routing table (a [partition](https://planetscale.com/learn/articles/sharding-vs-partitioning-whats-the-difference) conceptually).

BGP can be divided in the following components:

1. Input processing: decoding and validating messages received from each peer;
2. Routing table operations: modifying the routing table and determining the set of updates to generate;
3. Update encoding: draining the update queue and encoding messages to a set of peers;
4. Management operations: configuration processing and diagnostics.

For input processing concurrency can be acomplished by processing each peer individually. Routing table processing is parallelized by sharding the table with as many shards as the number of CPUs; an hash function is applied to the route prefix selecting the shard (or db partition). Update encoding is performed on a peer group basis; a peer is assigned to a peer group if it has the same policy when it comes to the attributes that it generates. Artificially we add cpu-affinity as a policy attribute in order to divide peers accross groups.

Test driven development
-----------------------

The goal for the BGP implementation is to have testing at:

- class interface;
- module (group of classes working together such as the update engine);
- process level;

Unit testing drives code organization. Classes should be designed such that their interface provides a service which can be tested as a unit. The best way to acomplish this is to follow the TDD principle of writing the test of the interface before the implementation.

The bgp implementation is instantiated via a single local BgpServer object in main(). From the server object there is a tree of all the other objects that componse the implementation. This allows the test code to run multiple BGP servers within the same process. Along with dependency injection, it also allows the test code to replace any class or set of classes when performing module or process level testing.

Dependency injection is acomplished via a [factory](https://github.com/OpenSDN-io/tf-common/blob/master/base/factory.h) mechanism, see [the docs index](https://opensdn-io.github.io/doxygen-docs/html/classes.html#letter_F). The [bgp factory](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_factory.h) configuration defines the default implementations for each interface. Test code can override the defaults by overriding the registration as with the following example:

    BgpObjectFactory::Register<BgpSessionManager>(boost::factory<BgpSessionManagerMock *>());

Unit tests are executed using the [Google C++ Testing Framework](http://code.google.com/p/googletest/). Mock objects are built either manually when trivial or by using the corresponding [Mocking Framework](http://code.google.com/p/googletest/). The BGP test suite can be executed via the build target src/bgp:test. Code coverage can be generated by passing the flag --optimization=coverage to the build tool (scons).

Input processing
----------------

BGP protocol messages are decoded and validated before being placed in the routing table operations queue. We use a mini-DSL, implemented as C++ templates, in order to decode and encode BGP messages.

This [DSL](https://github.com/OpenSDN-io/tf-common/blob/master/base/proto.h) is generic and can be used to implement encoding and decoding for different TLV-style protocols.

The BGP grammar is declared in [bgp_proto.cc](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_proto.cc). Each node in the grammar declares either a "Sequence" of elements to parse or a "Choice" between multiple elements.

Simple elements store and retrieve the message data to/from an in-memory datastructure that is then used internally. For example, the following grammar node stores the autonomous-system parameter in an OPEN message in the "as_num" field of an object of type "BgpProto::OpenMessage":

    class BgpOpenAsNum : public ProtoElement<BgpOpenAsNum> {
    public:
        static const int kSize = 2;
        typedef Accessor<BgpProto::OpenMessage, uint32_t, &BgpProto::OpenMessage::as_num> Setter;
        
    };

Complex elements (Sequences and Choices) can then include simple elements. The example element above is used in the open message definition with the following:

    class BgpOpenMessage : public ProtoSequence<BgpOpenMessage> {
    public:
        typedef mpl::list<BgpOpenVersion,
                          BgpOpenAsNum,
                          BgpHoldTime,
                          BgpIdentifier,
                          BgpOpenOptParam> Sequence;
        typedef BgpProto::OpenMessage ContextType;
    };

The objective behind this approach is to eliminate trivial mistakes such as reading beyond the message boundary. When TLV decoding is manually generated it is necessary to add simple validation checks through the parsing code for every single component. With an automated approach it is possible to ensure consistency.

An important requirement for the DSL is to allow the programmer to define the in-memory datastructure used to then store the resulting data. The concern is that the DSL should not force the programmer to then copy the data manually between a data structure that is defined by a library or code generator and a data structure that is convinient for the code that then processes the message.

Routing table operations
------------------------

Routing tables are in-memory datastructures that organized such that one module of code can manage the contents by performing add/change/delete operations and multiple modules can be "listeners" to table events. This is often referred to as the "observer pattern". Routing protocol implementation commonly use this approach; it is used by GateD and its derivatives for instance. OpenSDN implements this pattern via the [db](https://github.com/OpenSDN-io/tf-controller/tree/master/src/db) library which is used by several different modules like, for instance, the configuration management code in both the control-node and compute-node agent.

The OpenSDN [db](https://github.com/OpenSDN-io/tf-controller/tree/master/src/db) library supports the notion of having a DBTable that has multiple partitions (or shards) that correspond to multiple datasets. For BGP routing tables, the partition is determined by hashing the NLRI prefix. The hash function is such that it ignores the RD component of L3VPN routes. This guarantees that an L3VPN route is in the same partition as the corresponding IP route allowing for VRF import and export processing to be done in the context of the same partition (dataset).

Inbound messages are placed into a workqueue associated with a database partition. The workqueue deals with concurrency between Input processing and routing table stages. The table's Input method is called whenever there is work to be processed. It converts the incoming message into a table Add/Delete/Change operation.

The [db](https://github.com/OpenSDN-io/tf-controller/tree/master/src/db) library will then notify any listeners of the particular table and present them with with a change set. The change notification calls all listeners of a particular table in sequence. This is where operations such as "Update generation" and VRF import/export are performed.

Update generation
-----------------

For each routing table, the implementation creates a [RibOut](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_ribout.h) object for each different "Export Policy", where the concept of export policy is the set of factors that influence how updates are calculated. CPU affinity can also be part of export policy in order to force additional parellelization of update dequeuing.

Whenever there is a change in a routing table entry, the BgpExport::Export method is called for each [RibOut](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_ribout.h). This call occurs under the context of the "Routing Table Operations" task and respective table partition. The "Export" method calculates the desired attributes for the route and manipulates the update queue associated with the [RibOut](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_ribout.h).

The update manipulation code will ensure that there is a single update enqueued for an NLRI at a given time removing any previously enqueued updates for a prefix that is changing. Changes that are redundant are also supressed. This is achieved by comparing the desired state for the route to the state last advertised to the peer. Being able to suppress pending updates can reduce the number of messages sent to a slow receiver.

Update dequeing
---------------

BGP peers are assigned to a SchedulingGroup. This is a group of BGP peers which advertise the same [RibOut](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_ribout.h)s. A scheduling group can be processed in parallel with other scheduling groups.

Concurrency is managed by the [RibUpdateMonitor](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_update_monitor.h) class. The update queue has multiple producers (one per routing table partition) and a single consumer: the scheduling group task that dequeues updates.

Once an update is dequeued the corresponding message is formatted and written to the TCP socket of each peer. When a TCP socket is full, the respective peer is taken out of the current send set and a marker is added to the queue. Once the socket becomes available for writing again the send processing resumes from the last enqueued marker.

Configuration
-------------

When used in the control node process, BGP derives its internal configuration from the network configuration distributed by the IFMAP server. This functionality is performed by the [BgpConfigManager](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_config.h) class.

Unit tests often use an XML configuration parser to setup the desired state of the BGP server. The [BgpConfigParser](https://github.com/OpenSDN-io/tf-controller/blob/master/src/bgp/bgp_config_parser.h) class translates an XML file into a set of ifmap identity and metadata as expected by the configuration manager; tests can also replace the default config manager.

Diagnostics
-----------

The control-node process (which includes BGP) runs a web server on port 8083. BGP diagnostics information is available via this web interface.

