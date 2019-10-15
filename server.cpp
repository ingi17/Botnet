//
// Simple Botnet server for TSAM
//
// Command: ./tsamvgroup62 <serverPort>
// clientPort hardcoded as 4058
//
// Author: Ingi Thor Adalsteinsson
// Template: Jacky Mallett (jacky@ru.is)
//
// skel: 130.208.243.61

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>

#include <net/if.h>
#include <ifaddrs.h>

#include <iostream>
#include <sstream>
#include <thread>
#include <map>

#include <ctime>
#include <iostream>
#include <fstream>

#include <unistd.h>

using namespace std;

// fix SOCK_NONBLOCK for OSX
#ifndef SOCK_NONBLOCK
#include <fcntl.h>
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#define BACKLOG  5          // Allowed length of queue of waiting connections

// Simple class for handling connections from clients.
//
// Client(int socket) - socket to send/receive traffic from client.
class Client
{
  public:
    int sock;              // socket of client connection
    string name;           // Limit length of name of client's user

    Client(int socket) : sock(socket){} 

    ~Client(){}            // Virtual destructor defined for base class
};

// Class for handling connections from Servers,
class Server
{
    public:
        int sock;
        string ip;          // IP address of server
        string id;          // GROUP_ID of server
        string port;        // Port the server is connecting from

        vector<string> messages;    // Vector to hold messages for applicable server

    Server(int socket, string ip, string port)  // Constructor - id gets filled later
    {
        sock = socket;
        this->ip = ip;
        this->port = port;
    }

    ~Server(){}
};

const int clientPort = 4058;        // Port for listening to client connections
int serverPort = 0;                 // Server Port, this gets set from the command line argument

// Note: map is not necessarily the most efficient method to use here,
// especially for a server with large numbers of simulataneous connections,
// where performance is also expected to be an issue.
//
// Quite often a simple array can be used as a lookup table, 
// (indexed on socket no.) sacrificing memory for speed.

map<int, Client*> clients; // Lookup table for per Client information
map<int, Server*> servers;

// Open socket for specified port.
//
// Returns -1 if unable to create the socket for any reason.

int open_socket(int portno)
{
   struct sockaddr_in sk_addr;   // address settings for bind()
   int sock;                     // socket opened for this port
   int set = 1;                  // for setsockopt

   // Create socket for connection. Set to be non-blocking, so recv will
   // return immediately if there isn't anything waiting to be read.
#ifdef __APPLE__     
   if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
   {
      perror("Failed to open socket");
      return(-1);
   }
#else
   if((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
   {
     perror("Failed to open socket");
    return(-1);
   }
#endif

   // Turn on SO_REUSEADDR to allow socket to be quickly reused after 
   // program exit.

   if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
   {
      perror("Failed to set SO_REUSEADDR:");
   }
   set = 1;
#ifdef __APPLE__     
   if(setsockopt(sock, SOL_SOCKET, SOCK_NONBLOCK, &set, sizeof(set)) < 0)
   {
     perror("Failed to set SOCK_NOBBLOCK");
   }
#endif
   memset(&sk_addr, 0, sizeof(sk_addr));

   sk_addr.sin_family      = AF_INET;
   sk_addr.sin_addr.s_addr = INADDR_ANY;
   sk_addr.sin_port        = htons(portno);

   // Bind to socket to listen for connections from clients

   if(bind(sock, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
   {
      perror("Failed to bind to socket:");
      return(-1);
   }
   else
   {
      return(sock);
   }
}

// ------------------ Helper functions ------------------

// --------- String formatting functions
// Add token character 01 to start of string and 04 to end of string
string setSohEoh(string msg)
{
    msg = '\1' + msg + '\4';
    return msg;
}

// Remove token characters from string
vector<string> rmSohEoh(string msg)
{
    string tmp = msg;
    vector<string> clean;
    string token;
    string delim = "\4";
    size_t pos = 0;

    while((pos = tmp.find(delim)) != string::npos) {
        token = tmp.substr(0, pos);
        token = token.substr(1, pos);
        clean.push_back(token);
        tmp.erase(0, pos + delim.length());
    }
    /*
    for(auto cl : clean) {
        cout << "clean: " << cl << endl;
    }*/

    return clean;
}

// --------- Get local IP
string myIp()
{
    struct ifaddrs *myaddrs, *ifa;
    void *in_addr;
    char buf[64];
    string output;

    if(getifaddrs(&myaddrs) != 0)
    {
        perror("getifaddrs");
        exit(1);
    }

    for (ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        switch (ifa->ifa_addr->sa_family)
        {
            case AF_INET:
            {
                struct sockaddr_in *s4 = (struct sockaddr_in *)ifa->ifa_addr;
                in_addr = &s4->sin_addr;
                break;
            }
            // I don't like IPv6 >:(
            case AF_INET6:
            {
                continue;
            }
            default:
                continue;
        }
        
        if (!inet_ntop(ifa->ifa_addr->sa_family, in_addr, buf, sizeof(buf)))
        {
            printf("%s: inet_ntop failed!\n", ifa->ifa_name);
       }
        else if (string(buf).compare("127.0.0.1") != 0)
        {
            output = string(buf);
        }
    }

    freeifaddrs(myaddrs);
    return output;
}

// ------------------ Connection Functions ------------------

// Close a client's connection, remove it from the client list, and
// tidy up select sockets afterwards.
void closeClient(int clientSocket, fd_set *openSockets, int *maxfds)
{
     // Remove client from the clients list
     clients.erase(clientSocket);

     // If this client's socket is maxfds then the next lowest
     // one has to be determined. Socket fd's can be reused by the Kernel,
     // so there aren't any nice ways to do this.

     if(*maxfds == clientSocket)
     {
        for(auto const& p : clients)
        {
            *maxfds = std::max(*maxfds, p.second->sock);
        }
     }

     // And remove from the list of open sockets.

     FD_CLR(clientSocket, openSockets);
}

// Close connection from server. Almost identical to closeClient function
void closeServer(int servSocket, fd_set *openSockets, int *maxfds)
{
    printf("Server closed the connection: %d\n", servSocket);
    servers.erase(servSocket);

    if(*maxfds == servSocket)
    {
        for(auto const& p : servers)
        {
            *maxfds = max(*maxfds, p.second->sock);
        }
    }

    FD_CLR(servSocket, openSockets);
}

// --------- Connect to server
// Function for handling the clients CONNECT command. 
// Passes ip and port as parameters for designated server to connect to.
// Performs initial LISTSERVERS and SERVERS response handshake between servers.
// This could have been written a lot more cleanly, however it works like this and
// I don't dare touch it again.
void serverConnect(string server, string port)
{
    // Attempt connect to server:port
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(server.c_str());
    address.sin_port = htons(stoi(port));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(connect(sock, (struct sockaddr*) &address, sizeof(address)) < 0) {
        perror("Error: failed to connect");
        exit(0);
    }

    cout << "Connected to " << server << " on socket " << sock << endl;
    
    // Add server to servers map. GROUPID gets filled later 
    servers[sock] = new Server(sock, server, port);

    // Open new buffer to read initial LISTSERVERS
    char buffer[1025];
    read(sock, buffer, sizeof(buffer));
    vector<string> msg = rmSohEoh(buffer);
    cout << "SERVER: " << msg[0] << "\n" << endl;
    
    vector<string> tokens;
    string token;
    stringstream stream(msg[0]);

    while(getline(stream, token, ','))
        tokens.push_back(token);

    // Check if the server sent the correct LISTSERVERS reply
    if (tokens[0].compare("LISTSERVERS") == 0)
    {
        // Set GROUP_ID parameter for this server
        servers[sock]->id = tokens[1];

        // Reply with my known servers, starting with myself
        string listservers = "SERVERS,P3_GROUP_62,";
        listservers += myIp() + "," + to_string(serverPort) + ";";
        for(auto const& server : servers) {
            listservers += server.second->id + ",";
            listservers += server.second->ip + ",";
            listservers += server.second->port + ";";
        }
        listservers = setSohEoh(listservers);
        
        if (send(sock, listservers.c_str(), listservers.length(), 0) < 0) {
            printf("send failed");
        }

        cout << "SEND: " << listservers << endl;

        listservers = setSohEoh("LISTSERVERS,P3_GROUP_62");
        send(sock, listservers.c_str(), listservers.length(), 0);

        // Read SERVERS reply from the server
        read(sock, buffer, sizeof(buffer));
        //string msg = rmSohEoh(buffer);
        vector<string> msg = rmSohEoh(buffer);
        for (auto m : msg) {
            cout << "SERVER:" << m << "\n" << endl;
        }

    }
}

// ------------------ Command Functions ------------------

// Standalone function to handle the timed KEEPALIVE command.
// Keepalive is launched in a seperate thread and uses chrono to sleep
// the thread for 60 seconds before sending a KEEPALIVE command to all
// known servers, along with the amount of messages stored.
void keepAlive()
{
    string msg;
    while(true) {
        this_thread::sleep_for(chrono::seconds(60));
        for (auto const& server : servers) {
            // DEBUG: List all known servers every minute for debugging
            // cout << "SERVER INFO: " << server.second->sock << "," << server.second->id << "," << server.second->messages.size() << endl;
            msg = "KEEPALIVE,";
            msg = setSohEoh(msg + to_string(server.second->messages.size()));
            cout << "Sent KEEPALIVE," << server.second->messages.size() << " to " << server.second->id << endl;
            send(server.second->sock, msg.c_str(), msg.length(), 0);
        }
    }
}

// Saves messages sent to the server.
// FORMAT: SEND_MSG,<FROM_GROUP>,<TO_GROUP>,<MESSAGE>
// Adds FROM_GROUP,TO_GROUP,MESSAGE to the message vector of the designated server.
void SAVE_MSG(vector<string> tokens) {
    // TOKENS[1] = FROM_GROUP_ID
    // TOKENS[2] = TO_GROUP_ID
    // TOKENS[3] = MESSAGE_CONTENT
    string id, msg;
    for (auto server : servers) {
        id = server.second->id;
        if (id == tokens[2]) {
            msg = tokens[1] + "," + tokens[2] + "," + tokens[3];
            server.second->messages.push_back(msg);
        }
    }
}

// Requests messaged stored for myself from other servers.
// An additional groupID parameter could be passed to fetch messages for other groups
// although I don't see a case where I would need to grab someone else's messages
void GET_MSG(int sock) {
    string msg = "GET_MSG,P3_GROUP_62";
    cout << "SEND: " << msg << endl;
    msg = setSohEoh(msg);
    send(sock, msg.c_str(), msg.length(), 0);
}

// Send all messages I have stored to the server that requested it if the ID matches
void SEND_MSG(int sock, vector<string> tokens) {
    string id, msg;

    for (auto server : servers)
    {
        id = server.second->id;

        // Check if the id is the one corrected
        if (id == tokens[1]) {

            // Send and pop messages from the vector until its 0
            while(server.second->messages.size() != 0) {

                msg = "SEND_MSG,";
                msg += server.second->messages.back();  

                cout << "SEND: " << msg << endl;
                msg = setSohEoh(msg);

                send(sock, msg.c_str(), msg.length(), 0);

                server.second->messages.pop_back();
            }
        }
    }
}

// --------- Server Commands
// Function to handle incoming commands from other servers
void serverCommand(int sock, fd_set *openSockets, int *maxfds, string buffer)
{
    cout << "SERVER: " << buffer << endl;
    // Split incoming tokens on commas
    vector<string> tokens;
    string token;

    stringstream ss(buffer);

    while(getline(ss, token, ','))
        tokens.push_back(token);


    // LISTSERVERS
    // Response: SERVERS,<GROUP_ID>,<IP>,<Port>;
    // Starts with myself and then all other known servers
    if((tokens[0].compare("LISTSERVERS") == 0) && tokens.size() == 2)
    {
        servers[sock]->id = tokens[1];

        string msg = "SERVERS,";
        msg += "P3_GROUP_62," + myIp() + "," + to_string(serverPort) + ";";

        for (auto const& server : servers)
        {
            if (server.second->id.length() != 0)
            {
                msg += server.second->id + ",";
                msg += server.second->ip + ",";
                msg += server.second->port + ";";
            }
        }
        cout << "SEND: " << msg << endl;
        msg = setSohEoh(msg);
        send(sock, msg.c_str(), msg.length(), 0);
    }
    // SERVERS,<GROUP_ID>,<IP>,<Port>;
    // Response: N/A
    // Updates GROUP_ID for server on the applicable socket
    else if(tokens[0].compare("SERVERS") == 0) {
        servers[sock]->id = tokens[1];
    }
    // KEEPALIVE,<Stored_Messages>
    // Response: GET_MSG,<GROUP_ID>
    // Receiving KEEPALIVE alerts my server to keep the connection to
    // the other server alive and if there are any messages stored for me
    else if((tokens[0].compare("KEEPALIVE") == 0) && tokens.size() == 2) {
        // Check if there are any messages waiting
        if (stoi(tokens[1]) > 0) {
            // If so, get all the messages
            GET_MSG(sock);
        }
    }
    // GET_MSG,<GROUP_ID>
    // Response: SEND_MSG,<FROM_GROUP_ID>,<TO_GROUP_ID>,<MESSAGE_CONTENT>
    // Receiving GET_MSG instructs my server to send all messages for the GROUP_ID
    // to the applicable socket/server
    else if((tokens[0].compare("GET_MSG") == 0) && tokens.size() == 2){
        SEND_MSG(sock, tokens);
    }
    // SEND_MSG,<FROM_GROUP_ID>,<TO_GROUP_ID>,<MESSAGE_CONTENT>
    // RESPONSE: N/A
    // Stores message received in the applicable server vector
    else if((tokens[0].compare("SEND_MSG") == 0) && tokens.size() == 4) {
        cout << "SERVER: SEND MSG RECEIVED" << endl;
        SAVE_MSG(tokens);
    }
    // LEAVE
    // RESPONSE: N/A
    // The assignment instructs to make LEAVE,IP,PORT but that would let a server
    // tell my server to disconnect from another server, which doesn't make much sense to me
    // Decided to have leave just close the socket that sent the leave command.
    else if(tokens[0].compare("LEAVE") == 0) {
        closeServer(sock, openSockets, maxfds);
    }
    // STATUSREQ,<FROM_GROUP_ID>
    // RESPONSE: STATUSRESP,<GROUP_ID>,<Stored_Messages>
    // This command will never be received as the client has no way to instruct it
    // However it's still implemented. Simple for auto loop like almost all other functions
    else if((tokens[0].compare("STATUSREQ") == 0) && tokens.size() == 2) {
        string msg = "STATUSRESP,";
        for (auto const& server : servers) {
            msg += server.second->id + ",";
            msg += to_string(server.second->messages.size()) + ",";
        }
        cout << "SEND: " << msg << endl;
        msg = setSohEoh(msg);
        send(sock, msg.c_str(), msg.length(), 0);
    }
}

void writeToFile(string info)
{
    time_t current_time = time(NULL);
    ofstream outfile;
    outfile.open("info.log", ios::out);
    outfile << ctime(&current_time) << " " << info << endl;
    outfile.close();
}

// --------- Client Commands
// Process command from client to server
void clientCommand(int clientSocket, fd_set *openSockets, int *maxfds, 
                  char *buffer) 
{
  std::vector<std::string> tokens;
  std::string token;

  // Split command from client into tokens for parsing
  std::stringstream stream(buffer);
  while(getline(stream, token, ','))
        tokens.push_back(token);

  // CONNECT,<IP>,<PORT>
  // RESPONSE: N/A
  // Function not listed in the Assignment description but our client needs
  // a way to instruct the server to connect to another server
  if((tokens[0].compare("CONNECT") == 0) && (tokens.size() == 3))
  {
    serverConnect(tokens[1], tokens[2]);
  }
  // LISTSERVERS
  // RESPONSE: SERVERS,<GROUP_ID>,<IP>,<Port>;
  // Sends LISTSERVERS command to the server as if my client was a server
  // the response is then sent to the clientsocket instead of serversocket
  else if(strncmp(tokens[0].c_str(), "LISTSERVERS", 11) == 0)
  {
      string msg = setSohEoh("LISTSERVERS,P3_GROUP_62");
      serverCommand(clientSocket, openSockets, maxfds, msg);
  }
  // STATUSREQ
  // RESPONSE: N/A
  // Sends STATUSREQ to all known servers to test statusreq functionality
  else if(strncmp(tokens[0].c_str(), "STATUSREQ", 9) == 0){
      string msg = "STATUSREQ,P3_GROUP_62";
      for (auto const& server : servers) {
          send(server.second->sock, msg.c_str(), msg.length(), 0);
      }
  }
  // LEAVE
  // RESPONSE: N/A
  // Close the socket, and leave the socket handling code to deal with
  // tidying up clients etc. when select() detects the OS has torn down the connection.
  else if(tokens[0].compare("LEAVE") == 0)
  {
      closeClient(clientSocket, openSockets, maxfds);
  }
  // GETMSG,<GROUP_ID>
  // RESPONSE: <FROM_GROUP_ID>,<TO_GROUP_ID>,<MESSAGE_CONTENT>
  // Displays the latest comment assigned to the group ID parameter
  else if((tokens[0].compare("GETMSG") == 0) && tokens.size() == 2) {
      string msg;
      for (auto const& server : servers) {
          if (server.second->id == tokens[1]) {
            msg = server.second->messages.back();
            send(clientSocket, msg.c_str(), msg.length(), 0);
          }
      }
  }
  // SENDMSG,<GROUP_ID>,<MESSAGE_CONTENT>
  // RESPONSE: N/A
  // Sends a single message to the server to store for the GROUP_ID
  else if((tokens[0].compare("SENDMSG") == 0) && tokens.size() == 3) {
      string msg = "P3_GROUP_62,";
      msg += tokens[1] + ",";
      msg += tokens[2];
      for (auto server : servers) {
          if (server.second->id == tokens[1]) {
              server.second->messages.push_back(msg);
          }
      }
  }
  // KEEPALIVE
  // RESPONSE: N/A
  // Forces a keepalive message to all known servers. Mostly for debugging purposes
  else if(tokens[0].compare("KEEPALIVE") == 0)
  {
    string msg = "KEEPALIVE,";
    for (auto const& server : servers) {
        msg += server.second->messages.size();
        msg = setSohEoh(msg);
        // printf("Sent KEEPALIVE,%d to %s", server.second->messages.size(), server.second->id);
        send(server.second->sock, msg.c_str(), msg.length(), 0);
    }
  }
  else
  {
      std::cout << "Unknown command from client: " << buffer << std::endl;
  }
}

// ------------------ MAIN ------------------
int main(int argc, char* argv[])
{
    bool finished;
    int listenSock;                 // Socket for connections to server
    int clientSock;                 // Socket of connecting client
    int servSock;
    fd_set openSockets;             // Current open sockets 
    fd_set readSockets;             // Socket list for select()        
    fd_set exceptSockets;           // Exception socket list
    int maxfds;                     // Passed to select() as max fd in set
    struct sockaddr_in client;
    socklen_t clientLen;
    struct sockaddr_in server;
    socklen_t servLen;
    char buffer[1025];              // buffer for reading from clients

    // time_t t = std::time(NULL);     // Timestamp except for the part where it hardly functions

    if(argc != 2)
    {
        printf("Usage: tsamvgroup62 <serverPort>\n");
        exit(0);
    }

    serverPort = atoi(argv[1]);

    // Setup socket for server to listen to
    servSock = open_socket(atoi(argv[1]));
    //clientSock = open_socket(atoi(argv[2]));
    clientSock = open_socket(clientPort);
    printf("Listening on ports SERVER: %d CLIENT: %d\n", atoi(argv[1]), clientPort);

    if(listen(servSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", argv[1]);
        close(servSock);
        exit(0);
    } else {
        FD_ZERO(&openSockets);
        FD_SET(servSock, &openSockets);
        maxfds = servSock;
    }

    if(listen(clientSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %d\n", clientPort);
        close(clientSock);
        exit(0);
    }
    else 
    // Add listen socket to socket set we are monitoring
    {
        FD_SET(clientSock, &openSockets);
        maxfds = clientSock;
    }

    // Detach thread to call keepAlive every minute
    thread(keepAlive).detach();

    finished = false;

    while(!finished)
    {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openSockets;
        memset(buffer, 0, sizeof(buffer));

        // Look at sockets and see which ones have something to be read()
        int n = select(maxfds + 1, &readSockets, NULL, &exceptSockets, NULL);

        if(n < 0)
        {
            perror("select failed - closing down\n");
            finished = true;
        }
        else
        {
            // Server socket handling, almost identical to clientSocket
            if(FD_ISSET(servSock, &readSockets)) {

                listenSock = accept(servSock, (struct sockaddr *)&server, &servLen);
                printf("Server accept\n");

                cout << buffer << endl;

                struct in_addr ip_addr = server.sin_addr;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &ip_addr, ip, INET_ADDRSTRLEN);

                FD_SET(listenSock, &openSockets);

                maxfds = max(maxfds, listenSock);
                servers[listenSock] = new Server(listenSock, ip, to_string(server.sin_port));
                n--;
                
                string listserver = "LISTSERVERS,P3_GROUP_62";

                cout << "SEND: " << listserver << endl;
                listserver = setSohEoh(listserver);

                send(listenSock, listserver.c_str(), listserver.length(), 0);

                printf("Server IP %s connected on socket: %d\n", ip, servSock);
            }
            // First, accept  any new connections to the server on the listening socket
            if(FD_ISSET(clientSock, &readSockets))
            {
               listenSock = accept(clientSock, (struct sockaddr *)&client,
                                   &clientLen);
               printf("Client accept\n");
               // Add new client to the list of open sockets
               FD_SET(listenSock, &openSockets);

               // And update the maximum file descriptor
               maxfds = std::max(maxfds, listenSock) ;

               // create a new client to store information.
               clients[listenSock] = new Client(listenSock);

               // Decrement the number of sockets waiting to be dealt with
               n--;

               printf("Client connected on server: %d\n", listenSock);
            }
            while(n-- > 0)
            {
               // Now check for commands from clients
               for(auto const& pair : clients)
               {
                  Client *client = pair.second;

                  if(FD_ISSET(client->sock, &readSockets))
                  {
                      // recv() == 0 means client has closed connection
                      if(recv(client->sock, buffer, sizeof(buffer), MSG_DONTWAIT) == 0)
                      {
                          printf("Client closed connection: %d\n", client->sock);
                          close(client->sock);      
                          closeClient(client->sock, &openSockets, &maxfds);

                      }
                      // We don't check for -1 (nothing received) because select()
                      // only triggers if there is something on the socket for us.
                      else
                      {
                          // TIMESTAMP, except it works extremely awfully
                          // std::cout << "[" << ctime(&t) << "] " << "Client: " << buffer << std::endl;
                          std::cout << "Client: " << buffer << std::endl;
                          writeToFile(buffer);
                          clientCommand(client->sock, &openSockets, &maxfds, 
                                        buffer);
                      }
                  }
               }
               // Server command checking, identical to client
               for(auto const& pair : servers)
               {
                   Server *server = pair.second;
                   if(FD_ISSET(server->sock, &readSockets))
                   {
                       if(recv(server->sock, buffer, sizeof(buffer), MSG_DONTWAIT) == 0)
                       {
                            printf("Server closed the connection: %d\n", server->sock);
                            close(server->sock);
                            closeServer(server->sock, &openSockets, &maxfds);
                       }
                       else {
                           // TIMESTAMP, except it doesnt work very well
                           // cout << "[" << ctime(&t) << "] " << "Server: " << rmSohEoh(buffer) << endl;
                            vector<string> msg = rmSohEoh(buffer);
                            for (auto m : msg) {
                                serverCommand(server->sock, &openSockets, &maxfds, m);
                                writeToFile(m);
                            }
                       }
                   }
               }
            }
        }
    }
}