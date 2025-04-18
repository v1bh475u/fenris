#include "client/client.hpp"
#include "client/connection_manager.hpp"
#include "common/network_utils.hpp" // Include for common network helpers
#include "fenris.pb.h"

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// --- Helper Functions (Copied/Adapted from server tests) ---

// Helper function to serialize Request
std::vector<uint8_t> serialize_request(const fenris::Request &request)
{
    std::string serialized_data;
    request.SerializeToString(&serialized_data);
    return std::vector<uint8_t>(serialized_data.begin(), serialized_data.end());
}

// Helper function to deserialize Response
fenris::Response deserialize_response(const std::vector<uint8_t> &data)
{
    fenris::Response response;
    if (!response.ParseFromArray(data.data(), data.size())) {
        // Handle parse error, maybe return a default/error response or throw
        std::cerr << "Failed to parse response protobuf" << std::endl;
    }
    return response;
}

// Helper function to deserialize Request (for mock server)
fenris::Request deserialize_request(const std::vector<uint8_t> &data)
{
    fenris::Request request;
    if (!request.ParseFromArray(data.data(), data.size())) {
        std::cerr << "Failed to parse request protobuf" << std::endl;
    }
    return request;
}

// Helper function to serialize Response (for mock server)
std::vector<uint8_t> serialize_response(const fenris::Response &response)
{
    std::string serialized_data;
    response.SerializeToString(&serialized_data);
    return std::vector<uint8_t>(serialized_data.begin(), serialized_data.end());
}

// Helper function to send size-prefixed data
bool send_prefixed_data(int sock, const std::vector<uint8_t> &data)
{
    return fenris::common::network::send_size(sock, data.size(), false) &&
           fenris::common::network::send_data(sock, data, data.size(), false);
}

// Helper function to receive size-prefixed data
bool receive_prefixed_data(int sock, std::vector<uint8_t> &data)
{
    uint32_t size = 0;
    if (!fenris::common::network::receive_size(sock, size, false)) {
        return false;
    }
    // Basic size validation
    constexpr uint32_t MAX_REASONABLE_SIZE = 10 * 1024 * 1024; // 10 MB limit
    if (size == 0 || size > MAX_REASONABLE_SIZE) {
        std::cerr << "Invalid size received in receive_prefixed_data: " << size
                  << std::endl;
        return false;
    }
    data.resize(size);
    return fenris::common::network::receive_data(sock, data, size, false);
}

// --- Mock Server Implementation ---
class MockServer {
  public:
    MockServer()
        : m_port(0), m_listen_socket(-1), m_client_socket(-1), m_running(false)
    {
    }

    ~MockServer()
    {
        stop();
    }

    bool start()
    {
        m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_socket < 0) {
            perror("MockServer: socket creation failed");
            return false;
        }

        int yes = 1;
        setsockopt(m_listen_socket,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &yes,
                   sizeof(yes));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on any interface
        server_addr.sin_port = 0;                 // Assign ephemeral port

        if (bind(m_listen_socket,
                 (struct sockaddr *)&server_addr,
                 sizeof(server_addr)) < 0) {
            perror("MockServer: bind failed");
            close(m_listen_socket);
            m_listen_socket = -1;
            return false;
        }

        socklen_t len = sizeof(server_addr);
        if (getsockname(m_listen_socket,
                        (struct sockaddr *)&server_addr,
                        &len) == -1) {
            perror("MockServer: getsockname failed");
            close(m_listen_socket);
            m_listen_socket = -1;
            return false;
        }
        m_port = ntohs(server_addr.sin_port);

        if (listen(m_listen_socket, 1) < 0) {
            perror("MockServer: listen failed");
            close(m_listen_socket);
            m_listen_socket = -1;
            return false;
        }

        m_running = true;
        m_server_thread = std::thread(&MockServer::run, this);
        std::cout << "MockServer started on port " << m_port << std::endl;
        return true;
    }

    void stop()
    {
        if (!m_running.exchange(false)) {
            return; // Already stopped or never started
        }

        // Close sockets to unblock threads
        if (m_listen_socket != -1) {
            // Connect to self to unblock accept
            int wake_socket = socket(AF_INET, SOCK_STREAM, 0);
            if (wake_socket >= 0) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(m_port);
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                ::connect(wake_socket, (struct sockaddr *)&addr, sizeof(addr));
                close(wake_socket);
            }
            close(m_listen_socket); // Close listening socket
            m_listen_socket = -1;
        }
        {
            std::lock_guard<std::mutex> lock(m_client_mutex);
            if (m_client_socket != -1) {
                shutdown(m_client_socket,
                         SHUT_RDWR); // Forcefully shutdown client connection
                close(m_client_socket);
                m_client_socket = -1;
            }
        }

        if (m_server_thread.joinable()) {
            m_server_thread.join();
        }
        std::cout << "MockServer stopped" << std::endl;
    }

    int get_port() const
    {
        return m_port;
    }

    std::vector<fenris::Request> get_received_requests()
    {
        std::lock_guard<std::mutex> lock(m_requests_mutex);
        return m_received_requests;
    }

    void set_next_response(const fenris::Response &response)
    {
        std::lock_guard<std::mutex> lock(m_response_mutex);
        m_next_response = response;
    }

  private:
    void run()
    {
        std::cout << "MockServer thread running..." << std::endl;
        while (m_running) {
            struct sockaddr_storage client_addr;
            socklen_t sin_size = sizeof(client_addr);
            int temp_client_socket = accept(m_listen_socket,
                                            (struct sockaddr *)&client_addr,
                                            &sin_size);

            if (!m_running)
                break; // Check after accept returns

            if (temp_client_socket < 0) {
                if (errno != EBADF &&
                    errno !=
                        EINVAL) { // Ignore errors caused by closing the socket
                    perror("MockServer: accept failed");
                }
                continue;
            }

            std::cout << "MockServer accepted connection" << std::endl;
            {
                std::lock_guard<std::mutex> lock(m_client_mutex);
                m_client_socket = temp_client_socket; // Store the client socket
            }

            // Handle the single client connection
            handle_client(m_client_socket);

            // Clean up after client disconnects or handling stops
            {
                std::lock_guard<std::mutex> lock(m_client_mutex);
                if (m_client_socket != -1) {
                    close(m_client_socket);
                    m_client_socket = -1;
                }
            }
            std::cout << "MockServer client disconnected" << std::endl;
            // If the server should only handle one client and then stop
            // listening: m_running = false; // Optional: Stop after first
            // client
        }
        std::cout << "MockServer thread exiting." << std::endl;
    }

    void handle_client(int sock)
    {
        while (m_running) {
            std::vector<uint8_t> request_data;
            if (!receive_prefixed_data(sock, request_data)) {
                if (m_running) { // Only log error if server is supposed to be
                                 // running
                    std::cerr << "MockServer: Failed to receive request data "
                                 "or client disconnected."
                              << std::endl;
                }
                break; // Client disconnected or error
            }

            fenris::Request request = deserialize_request(request_data);
            {
                std::lock_guard<std::mutex> lock(m_requests_mutex);
                m_received_requests.push_back(request);
            }
            std::cout << "MockServer received request: " << request.command()
                      << std::endl;

            // Prepare and send response
            fenris::Response response_to_send;
            {
                std::lock_guard<std::mutex> lock(m_response_mutex);
                if (m_next_response.IsInitialized()) {
                    response_to_send = m_next_response;
                    m_next_response.Clear(); // Clear after use
                } else {
                    // Default response if none set
                    response_to_send.set_success(true);
                    response_to_send.set_type(fenris::ResponseType::PONG);
                    response_to_send.set_data("PONG");
                }
            }

            std::vector<uint8_t> response_data =
                serialize_response(response_to_send);
            if (!send_prefixed_data(sock, response_data)) {
                std::cerr << "MockServer: Failed to send response data."
                          << std::endl;
                break; // Error sending
            }
            std::cout << "MockServer sent response." << std::endl;

            // Special handling for TERMINATE
            if (request.command() == fenris::RequestType::TERMINATE) {
                std::cout
                    << "MockServer received TERMINATE, closing connection."
                    << std::endl;
                break; // Close connection after sending TERMINATE response
            }
        }
    }

    int m_port;
    int m_listen_socket;
    int m_client_socket; // Socket for the accepted client connection
    std::atomic<bool> m_running;
    std::thread m_server_thread;
    std::mutex m_client_mutex; // Mutex for client socket access

    std::vector<fenris::Request> m_received_requests;
    std::mutex m_requests_mutex;

    fenris::Response m_next_response;
    std::mutex m_response_mutex;
};

// --- Test Fixture ---
class ClientConnectionManagerTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        m_mock_server = std::make_unique<MockServer>();
        ASSERT_TRUE(m_mock_server->start());
        m_port = m_mock_server->get_port();
        m_port_str = std::to_string(m_port);

        // Create client connection manager instance
        m_connection_manager =
            std::make_unique<fenris::client::ConnectionManager>("127.0.0.1",
                                                                m_port_str);
        // m_connection_manager->set_non_blocking_mode(false); // Default is
        // blocking
    }

    void TearDown() override
    {
        if (m_connection_manager) {
            m_connection_manager->disconnect(); // Ensure client disconnects
        }
        if (m_mock_server) {
            m_mock_server->stop(); // Stop the mock server
        }
        // Short delay to allow sockets to fully close
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::unique_ptr<fenris::client::ConnectionManager> m_connection_manager;
    std::unique_ptr<MockServer> m_mock_server;
    int m_port;
    std::string m_port_str;
};

// --- Test Cases ---

TEST_F(ClientConnectionManagerTest, ConnectAndDisconnect)
{
    ASSERT_FALSE(m_connection_manager->is_connected());
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());

    // Allow time for server to accept
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    m_connection_manager->disconnect();
    ASSERT_FALSE(m_connection_manager->is_connected());
}

TEST_F(ClientConnectionManagerTest, ConnectionFailure)
{
    // Stop the server first
    m_mock_server->stop();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100)); // Ensure it's stopped

    ASSERT_FALSE(m_connection_manager->is_connected());
    // Try connecting to the now-stopped server
    ASSERT_FALSE(m_connection_manager->connect());
    ASSERT_FALSE(m_connection_manager->is_connected());
}

TEST_F(ClientConnectionManagerTest, SendRequest)
{
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());

    // Allow time for server to accept
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    fenris::Request ping_request;
    ping_request.set_command(fenris::RequestType::PING);
    ping_request.set_data("TestPing");

    ASSERT_TRUE(m_connection_manager->send_request(ping_request));

    // Give server time to receive and process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto received_requests = m_mock_server->get_received_requests();
    ASSERT_EQ(received_requests.size(), 1);
    ASSERT_TRUE(
        google::protobuf::util::MessageDifferencer::Equals(received_requests[0],
                                                           ping_request));

    m_connection_manager->disconnect();
}

TEST_F(ClientConnectionManagerTest, ReceiveResponse)
{
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());

    // Allow time for server to accept
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Prepare a response for the server to send
    fenris::Response expected_response;
    expected_response.set_success(true);
    expected_response.set_type(fenris::ResponseType::PONG);
    expected_response.set_data("TestPong");
    m_mock_server->set_next_response(expected_response);

    // Send a dummy request to trigger the response
    fenris::Request dummy_request;
    dummy_request.set_command(fenris::RequestType::PING);
    dummy_request.set_data("DummyPingData");
    ASSERT_TRUE(m_connection_manager->send_request(dummy_request));

    // Receive the response
    auto received_response_opt = m_connection_manager->receive_response();
    ASSERT_TRUE(received_response_opt.has_value());

    // Verify the response
    ASSERT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
        received_response_opt.value(),
        expected_response));

    m_connection_manager->disconnect();
}

TEST_F(ClientConnectionManagerTest, SendAndReceiveMultiple)
{
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());
    std::this_thread::sleep_for(
        std::chrono::milliseconds(50)); // Server accept time

    // --- Request 1 (PING) ---
    fenris::Request ping_request;
    ping_request.set_command(fenris::RequestType::PING);
    ping_request.set_data("Ping1");

    fenris::Response pong_response;
    pong_response.set_success(true);
    pong_response.set_type(fenris::ResponseType::PONG);
    pong_response.set_data("Pong1");
    m_mock_server->set_next_response(pong_response);

    ASSERT_TRUE(m_connection_manager->send_request(ping_request));
    auto received_pong_opt = m_connection_manager->receive_response();
    ASSERT_TRUE(received_pong_opt.has_value());
    ASSERT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
        received_pong_opt.value(),
        pong_response));

    // --- Request 2 (READ_FILE) ---
    fenris::Request read_request;
    read_request.set_command(fenris::RequestType::READ_FILE);
    read_request.set_filename("test.txt");

    fenris::Response file_response;
    file_response.set_success(true);
    file_response.set_type(fenris::ResponseType::FILE_CONTENT);
    file_response.set_data("File data");
    m_mock_server->set_next_response(file_response);

    ASSERT_TRUE(m_connection_manager->send_request(read_request));
    auto received_file_opt = m_connection_manager->receive_response();
    ASSERT_TRUE(received_file_opt.has_value());
    ASSERT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
        received_file_opt.value(),
        file_response));

    // Verify server received both requests
    std::this_thread::sleep_for(
        std::chrono::milliseconds(50)); // Ensure server processed
    auto received_requests = m_mock_server->get_received_requests();
    ASSERT_EQ(received_requests.size(), 2);
    ASSERT_TRUE(
        google::protobuf::util::MessageDifferencer::Equals(received_requests[0],
                                                           ping_request));
    ASSERT_TRUE(
        google::protobuf::util::MessageDifferencer::Equals(received_requests[1],
                                                           read_request));

    m_connection_manager->disconnect();
}

TEST_F(ClientConnectionManagerTest, ServerDisconnectDuringReceive)
{
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a request
    fenris::Request dummy_request;
    dummy_request.set_command(fenris::RequestType::PING);
    ASSERT_TRUE(m_connection_manager->send_request(dummy_request));

    // Stop the server abruptly *before* client tries to receive
    m_mock_server->stop();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100)); // Ensure server socket is closed

    // Attempt to receive - should fail and update connection status
    auto received_response_opt = m_connection_manager->receive_response();
    ASSERT_FALSE(received_response_opt.has_value());
    ASSERT_FALSE(
        m_connection_manager->is_connected()); // Should detect disconnection
}

// Add more tests:
// - ServerDisconnectDuringSend
// - SendRequestWhenNotConnected
// - ReceiveResponseWhenNotConnected
// - Test with non-blocking mode if relevant features depend on it

namespace fenris {
namespace client {
namespace tests {

} // namespace tests
} // namespace client
} // namespace fenris
