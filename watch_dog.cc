#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>


#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#if (defined(__APPLE__) && defined(__MACH__)) || defined(MAC_OS_X)
#include <netinet/in.h>
#endif // MAC_OS_X

const std::string CRLF("\r\n");
const std::string PROTOCOL_STR("http://");
const std::string URI_START("/");
const std::string HTTP_VERSION("HTTP/1.1");
const int HTTP_PORT = 80;
const std::string TEST_PREFIX("===> ");  // a prefix for test output, used in tests

/// scoped Socket class
class Socket {
    int socket;
  public:
    /// Constructor:
    /// A Socket is defined by a port number (16 bit), a protocol, and a domain, which
    /// defines a protocol family like ARPA Internet or ISO protocols.
    /// see "man socket"
    Socket(int domain, int type, int protocol) {
        socket = ::socket(domain, type, protocol);
    }

    /// Destructor:
    /// closes the socket if it is valid, i.e., if the constructor was successful
    virtual ~Socket() {
        if (-1 != socket)
            close(socket);
    }

    /// returns a socket descriptor
    int s() {
        return socket;
    }
};

/// A URL consisting of a host name and URI
struct URL {
    std::string host;
    std::string uri;
};

typedef std::vector<URL> URL_list;

// Reads URLs from a file and stores in std::vector
URL_list read_url_list(const std::string &url_file_name) {
    // the file name should be a command line argument
    std::ifstream url_file(url_file_name.c_str());

    std::string line;
    URL_list url_list;
    while (!url_file.eof()) {
        std::getline(url_file, line);
        if (line.length() > 0) {
            // if this line starts with the protocol we are interested in,
            // remove the protocol substring
            if (line.find(PROTOCOL_STR) == 0)
                line.erase(0, PROTOCOL_STR.length());

            // the hostname is separated by dots. After the first slash the URI begins
            int uri_start = line.find(URI_START);
            URL url;

            if (std::string::npos == uri_start) {
                // if no slash is found then the URI is "/" and the host is the whole string
                url.host = line;
                url.uri = URI_START;
            } else {
                // else we setup host and URI
                url.host = line.substr(0, uri_start);
                url.uri = line.substr(uri_start, line.length() - uri_start);
            }

            // insert new URL at the end of the URL list (std::vector)
            url_list.push_back(url);
        }
    }

    url_file.close();
    return url_list;
}

void test_server(const URL &url, int timeout) {
    // open socket as scoped object
    Socket http_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (-1 == http_socket.s()) {
        perror((TEST_PREFIX + "Could not create a socket").c_str());
        exit(1);
    }

    // try to set socket timeout using setsockopt, if not successful -> exit
    struct timeval timeout_val;
    timeout_val.tv_sec = timeout / 1000;
    timeout_val.tv_usec = (timeout % 1000) * 1000;
    if (0 != setsockopt(http_socket.s(), SOL_SOCKET, SO_SNDTIMEO, &timeout_val, sizeof(timeout_val))
        ||
            0 != setsockopt(http_socket.s(),
                            SOL_SOCKET,
                            SO_RCVTIMEO,
                            &timeout_val,
                            sizeof(timeout_val))) {
        perror((TEST_PREFIX + "Could set a socket timeout").c_str());
        exit(1);
    }

    // retrieve host IP address
    struct hostent *hostinfo;
    struct sockaddr_in host_addr;
    memset(&host_addr, 0, sizeof(host_addr));

    // we are using Internet address family (ARPA)
    host_addr.sin_family = AF_INET;

    // resolve hostname
    hostinfo = gethostbyname(url.host.c_str());
    if (NULL == hostinfo) {
        std::cerr << TEST_PREFIX << "Could not find host " << url.host << std::endl;
        perror((TEST_PREFIX + "\tReason").c_str());
        return;
    }
    memcpy(&(host_addr.sin_addr.s_addr), hostinfo->h_addr_list[0], sizeof(struct in_addr));

    // Internet byte order is "Most Significant Byte first",
    // x86 byte order is "Least Significant Byte first"
    // now we convert the port number: host to network short integer (htons)
    host_addr.sin_port = htons(HTTP_PORT);

    // try to connect
    if (0 != connect(http_socket.s(), (sockaddr *) &host_addr, sizeof(host_addr))) {
        std::cerr << TEST_PREFIX << "Could not connect to host " << url.host <<
                  "(" << inet_ntoa(host_addr.sin_addr) << ")" << std::endl;
        herror((TEST_PREFIX + "\tReason").c_str());
        return;
    }

    // write the HTTP GET request
    std::ostringstream request;
    request << "GET " << url.uri << " " << HTTP_VERSION << CRLF <<
            "host: " << url.host << CRLF << CRLF;
    std::string request_line(request.str());

    // if the length returned by the send operation
    // does not equal to the length of our request -> exit
    if (request_line.length() !=
        send(http_socket.s(), request_line.c_str(), request_line.length(), 0)) {
        std::cerr << TEST_PREFIX << "Could not send a request to host " << url.host <<
                  "(" << inet_ntoa(host_addr.sin_addr) << ")" << std::endl;
        perror((TEST_PREFIX + "\tReason").c_str());
        return;
    }

    // receive responses until first line feed (first line)
    std::ostringstream response;
    const int recv_buf_size = 100;
    char recv_buf[recv_buf_size + 1]; // last char is for \0
    do {
        int recv_length = recv(http_socket.s(), recv_buf, recv_buf_size, 0);
        if (-1 == recv_length) {
            std::cerr << TEST_PREFIX << "Could not receive a response from host " << url.host <<
                      "(" << inet_ntoa(host_addr.sin_addr) << ")" << std::endl;
            perror((TEST_PREFIX + "\tReason").c_str());
            return;
        }
        recv_buf[recv_length] = '\0';
        response << recv_buf;
    } while (NULL == strchr(recv_buf, CRLF[1]));

    // parse response
    std::string response_string = response.str();
    int pos = response_string.find(CRLF);
    std::string response_line(response_string.substr(0, pos));

    int start = response_line.find(' '), end;
    if (std::string::npos != start)
        end = response_line.find(' ', start + 1);
    if (std::string::npos == start || std::string::npos == end) {
        std::cerr << "Could not parse a response from host " << url.host <<
                  "(" << inet_ntoa(host_addr.sin_addr) << ")" << std::endl;
        perror("\tReason");
        return;
    }
    std::string status_code(response_line.substr(start, end - start + 1));

    // status code == 2xx: Success - The action was successfully received, understood, and accepted
    // else error
    if (atoi(status_code.c_str()) / 100 != 2) {
        std::cerr << TEST_PREFIX << "Host " << url.host << "(" << inet_ntoa(host_addr.sin_addr)
                  << ")" <<
                  " does not respond with \"success\"" << std::endl;
        std::cerr << TEST_PREFIX << "\tresponse line: " << response_line << std::endl;
        return;
    } else
        std::cout << TEST_PREFIX << "Successful response from host " << url.host <<
                  "(" << inet_ntoa(host_addr.sin_addr) << "): " <<
                  response_line << std::endl;
}

void usage(const char *err_msg = 0) {
    if (0 != err_msg)
        std::cerr << "Error: " << err_msg << std::endl;
    std::cout << "Usage: watch_dog <url_file> <timeout> <pause>" << std::endl <<
              "\t<url_file>: a text file with urls to monitor (one per line)" << std::endl <<
              "\t<timeout>: timeout for requests to the server, in ms" << std::endl <<
              "\t<pause>: pause before starting the next request, in ms" << std::endl;
    exit(1);
}
void printProcessId(pid_t pid, std::string name = "") {
    std::cout << "Process ID: " << pid;
    if (!name.empty()) {
        std::cout << ", Name: " << name;
    }
    std::cout << std::endl;
}

void watchDogSurvey(const URL_list &url_list, int timeout, int pause, int* next_url_pos) {
    int i = *next_url_pos;
    std::cout<<"Next URL position: "<<i<<std::endl;
    while (true) {
        for ( i= i ; i < url_list.size(); i++) {
            std::cout<<"Current URL position: "<<i<<std::endl;
            test_server(url_list[i], timeout);
            *next_url_pos = i + 1;
            usleep(pause * 1000);
        }
        i=0;
    }
}

int main(const int argc, const char **argv) {
    printProcessId(getpid(), "watch_dog");
    if (argc < 4)
        usage("Wrong number of command line arguments");

    std::string url_file(argv[1]);
    int timeout = atoi(argv[2]);
    int pause = atoi(argv[3]);
    std::cout << "URL file = " << url_file << std::endl <<
              "timeout = " << timeout << " ms" << std::endl <<
              "pause = " << pause << " ms" << std::endl;

    // read URL list from file
    URL_list url_list = read_url_list(url_file);

    // print all hosts and URIs
    for (URL_list::iterator i = url_list.begin(); i != url_list.end(); i++) {
        std::cout << "host = " << (i->host) << "; uri = " << (i->uri) << std::endl;
    }

    int* next_url_pos = (int*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (next_url_pos == MAP_FAILED) {
        std::cerr << "Error in mmap() for shared memory.\n";
        return 1;
    }

    // Initialize the current position
    *next_url_pos = 0;
    while(true){
        std::cout << "Starting new worker...\n";
        pid_t worker_pid = fork();
        if (worker_pid < 0){
            // Fork failed
            std::cerr << "Error: Fork failed.\n";
            std::cout<< "Watchdog taking over as worker...\n";
            //graceful degradation
            watchDogSurvey(url_list, timeout, pause,next_url_pos);
            return 1;
        }
        else if(worker_pid == 0){
            // Child process
            printProcessId(getpid(), "worker");
            // start watch dog survey
            watchDogSurvey(url_list, timeout, pause, next_url_pos); 
            
        }
        else{
            // Parent process
            printProcessId(getpid(), "watch_dog");
            int status;
            pid_t terminatedWorkerPid = waitpid(worker_pid, &status, 0);
            if(terminatedWorkerPid > 0)
            {
                if (WIFEXITED(status)) {
                    std::cout << "Worker process (PID: " << worker_pid << ") exited with code: " << WEXITSTATUS(status) << ".\n";
                } else if (WIFSIGNALED(status)) {
                    std::cout << "Worker process (PID: " << worker_pid << ") was killed by signal: " << WTERMSIG(status) << ".\n";
                }
            }else{
                std::cerr << "Error: waitpid failed.\n";
                return 1;
            }
        }
    }
    if (munmap(next_url_pos, sizeof(int)) == -1) {
        std::cerr << "Error: munmap() failed" << std::endl;
        return 1;
    }

    std::cout << "Shared memory cleaned up." << std::endl;

    // unreachable
    return 0;
}
