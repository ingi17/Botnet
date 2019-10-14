# Botnet
A simple botnet program with accompanying C&C clients.

### Prerequisites
G++ is required and make is optional to use the makefile.  
    ``sudo apt install g++``  
    ``sudo apt install make``  
    

### Compile
To compile run the Makefile with make or the following g++ command.  
    ``make``  
    ``g++ -Wall -std=c++11 client.cpp -lpthread -o client g++ -Wall -std=c++11 server.cpp -lpthread -o server``  

### Run
The following commands are used to run the server and client respectively.  
    ``./server <serverPort>``  
    ``./client <IP of server> <port>``  
If you wish to run it locally use the loopback address when running the client (127.0.0.1).  
NOTE: Assignment requested the server to be run with only the serverport parameter alone. Therefore port 4058 is hardcoded as the client port.

### Commands
From the client window the following commands are available:  
    ``CONNECT,<IP>,<PORT>             - Instructs server to connect to another server``  
    ``GETMSG,<GROUP_ID>               - Reads a single message for the GROUP_ID``  
    ``SENDMSG,<GROUP_ID>,<MESSAGE>    - Send a message to the server for the GROUP_ID``  
    ``LISTSERVERS                     - List all servers your server is connected to``  

