#include "client/client.hpp"
#include "client/connection_manager.hpp"
#include "common/crypto_manager.hpp"
#include "common/network_utils.hpp"
#include "common/request.hpp"
#include "common/response.hpp"
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

namespace fenris {
namespace client {
namespace tests {

using namespace fenris::common;
using namespace fenris::common::network;
using namespace google::protobuf::util;

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
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = 0;

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
            return;
        }

        if (m_listen_socket != -1) {
            int wake_socket = socket(AF_INET, SOCK_STREAM, 0);
            if (wake_socket >= 0) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(m_port);
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                ::connect(wake_socket, (struct sockaddr *)&addr, sizeof(addr));
                close(wake_socket);
            }
            close(m_listen_socket);
            m_listen_socket = -1;
        }
        {
            std::lock_guard<std::mutex> lock(m_client_mutex);
            if (m_client_socket != -1) {
                shutdown(m_client_socket, SHUT_RDWR);
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
                if (errno != EBADF && errno != EINVAL) {
                    perror("MockServer: accept failed");
                }
                continue;
            }

            std::cout << "MockServer accepted connection" << std::endl;
            {
                std::lock_guard<std::mutex> lock(m_client_mutex);
                m_client_socket = temp_client_socket;
            }

            handle_client(m_client_socket);

            {
                std::lock_guard<std::mutex> lock(m_client_mutex);
                if (m_client_socket != -1) {
                    close(m_client_socket);
                    m_client_socket = -1;
                }
            }
            std::cout << "MockServer client disconnected" << std::endl;
        }
        std::cout << "MockServer thread exiting." << std::endl;
    }

    void handle_client(uint32_t socket)
    {
        // First handle key exchange
        std::vector<uint8_t> encryption_key;

        if (!perform_key_exchange(socket, encryption_key)) {
            std::cerr << "MockServer: Key exchange failed" << std::endl;
            return;
        }

        std::cout << "MockServer: Key exchange successful" << std::endl;

        while (m_running) {
            auto request_opt = receive_request(socket, encryption_key);
            if (!request_opt.has_value()) {
                std::cerr << "MockServer: Failed to receive request."
                          << std::endl;
                break;
            }

            const fenris::Request &request = request_opt.value();

            {
                std::lock_guard<std::mutex> lock(m_requests_mutex);
                m_received_requests.push_back(request);
            }
            std::cout << "MockServer received request: " << request.command()
                      << std::endl;

            fenris::Response response_to_send;
            {
                std::lock_guard<std::mutex> lock(m_response_mutex);
                if (m_next_response.IsInitialized()) {
                    response_to_send = m_next_response;
                    m_next_response.Clear();
                } else {
                    response_to_send.set_success(true);
                    response_to_send.set_type(fenris::ResponseType::PONG);
                    response_to_send.set_data("PONG");
                }
            }

            if (!send_response(socket, encryption_key, response_to_send)) {
                std::cerr << "MockServer: Failed to send response."
                          << std::endl;
                break;
            }

            // Special handling for TERMINATE
            if (request.command() == fenris::RequestType::TERMINATE) {
                std::cout
                    << "MockServer received TERMINATE, closing connection."
                    << std::endl;
                break;
            }
        }
    }

    // Perform key exchange with the client
    bool perform_key_exchange(uint32_t socket,
                              std::vector<uint8_t> &encryption_key)
    {
        common::crypto::CryptoManager crypto_manager;

        // Generate ECDH keypair for the server
        auto [private_key, public_key, keygen_result] =
            crypto_manager.generate_ecdh_keypair();
        if (keygen_result != common::crypto::ECDHResult::SUCCESS) {
            std::cerr << "Failed to generate server ECDH keypair: "
                      << ecdh_result_to_string(keygen_result) << std::endl;
            return false;
        }

        // Receive the client's public key size
        std::vector<uint8_t> client_public_key;
        NetworkResult recv_result =
            receive_prefixed_data(socket, client_public_key);
        if (recv_result != NetworkResult::SUCCESS) {
            std::cerr << "failed to receive client public key: "
                      << network_result_to_string(recv_result) << std::endl;
            return false;
        }

        // Send our public key to the client
        NetworkResult send_result = send_prefixed_data(socket, public_key);
        if (send_result != NetworkResult::SUCCESS) {
            std::cerr << "failed to send server public key: "
                      << network_result_to_string(send_result) << std::endl;
            return false;
        }

        // Compute the shared secret
        auto [shared_secret, ss_result] =
            crypto_manager.compute_ecdh_shared_secret(private_key,
                                                      client_public_key);
        if (ss_result != common::crypto::ECDHResult::SUCCESS) {
            std::cerr << "failed to compute shared secret: "
                      << ecdh_result_to_string(ss_result) << std::endl;
            return false;
        }

        // Derive the encryption key
        auto [derived_key, key_derive_result] =
            crypto_manager.derive_key_from_shared_secret(
                shared_secret,
                crypto::AES_GCM_KEY_SIZE);
        if (key_derive_result != common::crypto::ECDHResult::SUCCESS) {
            std::cerr << "failed to derive key from shared secret: "
                      << ecdh_result_to_string(key_derive_result) << std::endl;
            return false;
        }

        encryption_key = derived_key;
        return true;
    }

    bool send_response(uint32_t socket,
                       const std::vector<uint8_t> &encryption_key,
                       const fenris::Response &response)
    {
        // Serialize the response
        std::vector<uint8_t> serialized_response = serialize_response(response);

        // Generate random IV
        auto [iv, iv_gen_result] = m_crypto_manager.generate_random_iv();
        if (iv_gen_result != crypto::EncryptionResult::SUCCESS) {
            return false;
        }

        // Encrypt the serialized response using client's key and generated IV
        auto [encrypted_response, encrypt_result] =
            m_crypto_manager.encrypt_data(serialized_response,
                                          encryption_key,
                                          iv);
        if (encrypt_result != crypto::EncryptionResult::SUCCESS) {
            return false;
        }

        // Create the final message with IV prefixed to encrypted data
        std::vector<uint8_t> message_with_iv;
        message_with_iv.reserve(iv.size() + encrypted_response.size());
        message_with_iv.insert(message_with_iv.end(), iv.begin(), iv.end());
        message_with_iv.insert(message_with_iv.end(),
                               encrypted_response.begin(),
                               encrypted_response.end());

        // Send the IV-prefixed encrypted response

        NetworkResult send_result = send_prefixed_data(socket, message_with_iv);
        if (send_result != NetworkResult::SUCCESS) {
            return false;
        }

        return true;
    }

    std::optional<fenris::Request>
    receive_request(uint32_t socket, const std::vector<uint8_t> &encryption_key)
    {
        // Receive encrypted data (includes IV + encrypted request)
        std::vector<uint8_t> encrypted_data;
        NetworkResult recv_result =
            receive_prefixed_data(socket, encrypted_data);

        if (recv_result != NetworkResult::SUCCESS) {
            return std::nullopt;
        }

        if (encrypted_data.size() < crypto::AES_GCM_IV_SIZE) {
            return std::nullopt;
        }

        // Extract IV from the beginning of the message
        std::vector<uint8_t> iv(encrypted_data.begin(),
                                encrypted_data.begin() +
                                    crypto::AES_GCM_IV_SIZE);

        // Extract the encrypted request data (after the IV)
        std::vector<uint8_t> encrypted_request(encrypted_data.begin() +
                                                   crypto::AES_GCM_IV_SIZE,
                                               encrypted_data.end());

        // Decrypt the request using client's key and extracted IV
        auto [decrypted_data, decrypt_result] =
            m_crypto_manager.decrypt_data(encrypted_request,
                                          encryption_key,
                                          iv);
        if (decrypt_result != crypto::EncryptionResult::SUCCESS) {
            return std::nullopt;
        }

        return deserialize_request(decrypted_data);
    }

    int m_port;
    int m_listen_socket;
    int m_client_socket;
    std::atomic<bool> m_running;
    std::thread m_server_thread;
    std::mutex m_client_mutex;

    std::vector<fenris::Request> m_received_requests;
    std::mutex m_requests_mutex;

    fenris::Response m_next_response;
    std::mutex m_response_mutex;
    crypto::CryptoManager m_crypto_manager;
};

class ClientConnectionManagerTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        m_mock_server = std::make_unique<MockServer>();
        ASSERT_TRUE(m_mock_server->start());
        m_port = m_mock_server->get_port();
        m_port_str = std::to_string(m_port);

        m_connection_manager =
            std::make_unique<fenris::client::ConnectionManager>(
                "127.0.0.1",
                m_port_str,
                "TestClientConnectionManager");
    }

    void TearDown() override
    {
        if (m_connection_manager) {
            m_connection_manager->disconnect();
        }
        if (m_mock_server) {
            m_mock_server->stop();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::unique_ptr<fenris::client::ConnectionManager> m_connection_manager;
    std::unique_ptr<MockServer> m_mock_server;
    int m_port;
    std::string m_port_str;
};

TEST_F(ClientConnectionManagerTest, ConnectAndDisconnect)
{
    ASSERT_FALSE(m_connection_manager->is_connected());
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    m_connection_manager->disconnect();
    ASSERT_FALSE(m_connection_manager->is_connected());
}

TEST_F(ClientConnectionManagerTest, ConnectionFailure)
{
    m_mock_server->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_FALSE(m_connection_manager->is_connected());
    // Try connecting to the now-stopped server
    ASSERT_FALSE(m_connection_manager->connect());
    ASSERT_FALSE(m_connection_manager->is_connected());
}

TEST_F(ClientConnectionManagerTest, SendRequest)
{
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    fenris::Request ping_request;
    ping_request.set_command(fenris::RequestType::PING);
    ping_request.set_data("TestPing");

    ASSERT_TRUE(m_connection_manager->send_request(ping_request));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto received_requests = m_mock_server->get_received_requests();
    ASSERT_EQ(received_requests.size(), 1);
    ASSERT_TRUE(MessageDifferencer::Equals(received_requests[0], ping_request));

    m_connection_manager->disconnect();
}

TEST_F(ClientConnectionManagerTest, ReceiveResponse)
{
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    fenris::Response expected_response;
    expected_response.set_success(true);
    expected_response.set_type(fenris::ResponseType::PONG);
    expected_response.set_data("TestPong");
    m_mock_server->set_next_response(expected_response);

    fenris::Request dummy_request;
    dummy_request.set_command(fenris::RequestType::PING);
    dummy_request.set_data("DummyPingData");
    ASSERT_TRUE(m_connection_manager->send_request(dummy_request));

    auto received_response_opt = m_connection_manager->receive_response();
    ASSERT_TRUE(received_response_opt.has_value());

    ASSERT_TRUE(MessageDifferencer::Equals(received_response_opt.value(),
                                           expected_response));

    m_connection_manager->disconnect();
}

TEST_F(ClientConnectionManagerTest, SendAndReceiveMultiple)
{
    ASSERT_TRUE(m_connection_manager->connect());
    ASSERT_TRUE(m_connection_manager->is_connected());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    ASSERT_TRUE(
        MessageDifferencer::Equals(received_pong_opt.value(), pong_response));

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
    ASSERT_TRUE(
        MessageDifferencer::Equals(received_file_opt.value(), file_response));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto received_requests = m_mock_server->get_received_requests();
    ASSERT_EQ(received_requests.size(), 2);
    ASSERT_TRUE(MessageDifferencer::Equals(received_requests[0], ping_request));
    ASSERT_TRUE(MessageDifferencer::Equals(received_requests[1], read_request));

    m_connection_manager->disconnect();
}

} // namespace tests
} // namespace client
} // namespace fenris
