// Copyright Takatoshi Kondo 2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <iostream>
#include <iomanip>
#include <set>

#include <mqtt_server_cpp.hpp>

#include <boost/lexical_cast.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

namespace mi = boost::multi_index;

using con_t = mqtt::server_tls_ws<>::endpoint_t;
using con_sp_t = std::shared_ptr<con_t>;

struct sub_con {
    sub_con(std::string topic, con_sp_t con, std::uint8_t qos)
        :topic(std::move(topic)), con(std::move(con)), qos(qos) {}
    std::string topic;
    con_sp_t con;
    std::uint8_t qos;
};

struct tag_topic {};
struct tag_con {};

using mi_sub_con = mi::multi_index_container<
    sub_con,
    mi::indexed_by<
        mi::ordered_non_unique<
            mi::tag<tag_topic>,
            BOOST_MULTI_INDEX_MEMBER(sub_con, std::string, topic)
        >,
        mi::ordered_non_unique<
            mi::tag<tag_con>,
            BOOST_MULTI_INDEX_MEMBER(sub_con, con_sp_t, con)
        >
    >
>;


inline void close_proc(std::set<con_sp_t>& cons, mi_sub_con& subs, con_sp_t const& con) {
    cons.erase(con);

    auto& idx = subs.get<tag_con>();
    auto r = idx.equal_range(con);
    idx.erase(r.first, r.second);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cout << argv[0] << " port server_crt server_key" << std::endl;
        return -1;
    }
    boost::asio::io_service ios;

    std::uint16_t port = boost::lexical_cast<std::uint16_t>(argv[1]);
    std::string cert = argv[2];
    std::string key = argv[3];

    boost::asio::ssl::context  ctx(boost::asio::ssl::context::tlsv12);
    ctx.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::single_dh_use);
    ctx.use_certificate_file(cert, boost::asio::ssl::context::pem);
    ctx.use_private_key_file(key, boost::asio::ssl::context::pem);

    auto s = mqtt::server_tls_ws<>(
        boost::asio::ip::tcp::endpoint(
            boost::asio::ip::tcp::v4(),
            port
        ),
        std::move(ctx),
        ios
    );

    s.set_error_handler(
        [](boost::system::error_code const& ec) {
            std::cout << "error: " << ec.message() << std::endl;
        }
    );

    std::set<con_sp_t> connections;
    mi_sub_con subs;

    s.set_accept_handler(
        [&](con_t& ep) {
            using packet_id_t = typename std::remove_reference_t<decltype(ep)>::packet_id_t;
            std::cout << "accept" << std::endl;
            auto sp = ep.shared_from_this();
            ep.start_session(
                [sp] // keeping ep's lifetime as sp until session finished
                (boost::system::error_code const& ec) {
                    std::cout << "session end: " << ec.message() << std::endl;
                }
            );

            // set connection (lower than MQTT) level handlers
            ep.set_close_handler(
                [&]
                (){
                    std::cout << "closed." << std::endl;
                    close_proc(connections, subs, ep.shared_from_this());
                });
            ep.set_error_handler(
                [&]
                (boost::system::error_code const& ec){
                    std::cout << "error: " << ec.message() << std::endl;
                    close_proc(connections, subs, ep.shared_from_this());
                });

            // set MQTT level handlers
            ep.set_connect_handler(
                [&]
                (mqtt::buffer client_id,
                 mqtt::optional<mqtt::buffer> username,
                 mqtt::optional<mqtt::buffer> password,
                 mqtt::optional<mqtt::will>,
                 bool clean_session,
                 std::uint16_t keep_alive) {
                    std::cout << "client_id    : " << client_id << std::endl;
                    std::cout << "username     : " << (username ? username.value() : mqtt::buffer("none")) << std::endl;
                    std::cout << "password     : " << (password ? password.value() : mqtt::buffer("none")) << std::endl;
                    std::cout << "clean_session: " << std::boolalpha << clean_session << std::endl;
                    std::cout << "keep_alive   : " << keep_alive << std::endl;
                    connections.insert(ep.shared_from_this());
                    ep.connack(false, mqtt::connect_return_code::accepted);
                    return true;
                }
            );
            ep.set_disconnect_handler(
                [&]
                (){
                    std::cout << "disconnect received." << std::endl;
                    close_proc(connections, subs, ep.shared_from_this());
                });
            ep.set_puback_handler(
                [&]
                (packet_id_t packet_id){
                    std::cout << "puback received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_pubrec_handler(
                [&]
                (packet_id_t packet_id){
                    std::cout << "pubrec received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_pubrel_handler(
                [&]
                (packet_id_t packet_id){
                    std::cout << "pubrel received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_pubcomp_handler(
                [&]
                (packet_id_t packet_id){
                    std::cout << "pubcomp received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_publish_handler(
                [&]
                (std::uint8_t header,
                 mqtt::optional<packet_id_t> packet_id,
                 mqtt::buffer topic_name,
                 mqtt::buffer contents){
                    std::uint8_t qos = mqtt::publish::get_qos(header);
                    bool retain = mqtt::publish::is_retain(header);
                    std::cout << "publish received."
                              << " dup: " << std::boolalpha << mqtt::publish::is_dup(header)
                              << " qos: " << mqtt::qos::to_str(qos)
                              << " retain: " << retain << std::endl;
                    if (packet_id)
                        std::cout << "packet_id: " << *packet_id << std::endl;
                    std::cout << "topic_name: " << topic_name << std::endl;
                    std::cout << "contents: " << contents << std::endl;
                    auto const& idx = subs.get<tag_topic>();
                    auto r = idx.equal_range(std::string(topic_name));
                    for (; r.first != r.second; ++r.first) {
                        r.first->con->publish(
                            std::string(topic_name),
                            std::string(contents),
                            std::min(r.first->qos, qos),
                            retain
                        );
                    }
                    return true;
                });
            ep.set_subscribe_handler(
                [&]
                (packet_id_t packet_id,
                 std::vector<std::tuple<mqtt::buffer, std::uint8_t>> entries) {
                    std::cout << "subscribe received. packet_id: " << packet_id << std::endl;
                    std::vector<std::uint8_t> res;
                    res.reserve(entries.size());
                    for (auto const& e : entries) {
                        mqtt::buffer topic = std::get<0>(e);
                        std::uint8_t qos = std::get<1>(e);
                        std::cout << "topic: " << topic  << " qos: " << static_cast<int>(qos) << std::endl;
                        res.emplace_back(qos);
                        subs.emplace(std::string(topic), ep.shared_from_this(), qos);
                    }
                    ep.suback(packet_id, res);
                    return true;
                }
            );
            ep.set_unsubscribe_handler(
                [&]
                (packet_id_t packet_id,
                 std::vector<mqtt::buffer> topics) {
                    std::cout << "unsubscribe received. packet_id: " << packet_id << std::endl;
                    for (auto const& topic : topics) {
                        subs.erase(std::string(topic));
                    }
                    ep.unsuback(packet_id);
                    return true;
                }
            );
        }
    );

    s.listen();

    ios.run();
}
