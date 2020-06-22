# oscreceiver_class

OSC receiver class using oscpack library.

To be added to your proyect as a git submodule and compiled all together with it.

In your code just make your class inherit from the OscReceiver class and code your own virtual function to process messages:

  inline virtual void ProcessMessage( const osc::ReceivedMessage& /*m*/ , const IpEndpointName& /*remoteEndpoint*/ ) {};
