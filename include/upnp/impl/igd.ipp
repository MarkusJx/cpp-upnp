
#include <boost/asio/ip/tcp.hpp>
#include <upnp/detail/local_address_to.h>
#include <upnp/ssdp.h>
#include <upnp/device.h>
#include <upnp/config.h>
#include <iostream>

namespace upnp {

inline
igd::igd( std::string   uuid
        , device        upnp_device
        , std::string   service_id
        , url_t         url
        , std::string   urn
        , net::executor exec)
    : _uuid(std::move(uuid))
    , _upnp_device(std::move(upnp_device))
    , _service_id(std::move(service_id))
    , _url(std::move(url))
    , _urn(std::move(urn))
    , _exec(exec)
{}

inline
result<void, igd::error::add_port_mapping>
igd::add_port_mapping( protocol proto
                     , uint16_t external_port
                     , uint16_t internal_port
                     , string_view description
                     , std::chrono::seconds duration
                     , net::yield_context yield) noexcept
{
    using namespace std::chrono;

    auto host_port = _url.host_and_port();
    auto opt_remote_ep = str::consume_endpoint<net::ip::tcp>(host_port);
    if (!opt_remote_ep)
        return error::igd_host_parse_failed{};

    auto opt_local_ip = local_address_to(*opt_remote_ep);
    if (!opt_local_ip)
        return error::no_endpoint_to_igd{};

    net::ip::address local_ip = *opt_local_ip;

    std::stringstream body;
    body << "<u:AddPortMapping xmlns:u=\""<< _urn <<"\">"
            "<NewRemoteHost></NewRemoteHost>"
            "<NewEnabled>1</NewEnabled>"
            "<NewExternalPort>"           << external_port     << "</NewExternalPort>"
            "<NewProtocol>"               << proto             << "</NewProtocol>"
            "<NewInternalPort>"           << internal_port     << "</NewInternalPort>"
            "<NewInternalClient>"         << local_ip          << "</NewInternalClient>"
            "<NewPortMappingDescription>" << description       << "</NewPortMappingDescription>"
            "<NewLeaseDuration>"          << duration.count()  << "</NewLeaseDuration>"
            "<NewLeaseDuration>"          << 0  << "</NewLeaseDuration>"
            "</u:AddPortMapping>";

    auto rs = soap_request("AddPortMapping", body.str(), yield);
    if (!rs) return error::soap_request{};

    auto result = rs.value().result();
    if (result != beast::http::status::ok) {
        return error::bad_response_status{result};
    }

    return success();
}

inline
result<net::ip::address, igd::error::get_external_address>
igd::get_external_address(net::yield_context yield) noexcept
{
    std::string body =
        "<u:GetExternalIPAddress xmlns:u\"urn:schemas-upnp-org:service:WANIPConnection:1\"/>";

    auto rs = soap_request("GetExternalIPAddress", body, yield);
    if (!rs) return error::soap_request{};

    auto result = rs.value().result();
    if (result != beast::http::status::ok) {
        return error::bad_response_status{result};
    }

    auto opt_xml = xml::parse(rs.value().body());
    if (!opt_xml) return error::invalid_xml_body{};
    auto& xml_rs = *opt_xml;

    const char* path = "s:Envelope.s:Body.u:GetExternalIPAddressResponse."
                       "NewExternalIPAddress";

    auto opt_ip_s = xml_rs.get_optional<std::string>(path);
    if (!opt_ip_s) return error::bad_result{};
    auto& ip_s = *opt_ip_s;

    sys::error_code ec;
    auto addr = net::ip::make_address(ip_s, ec);
    if (ec) return error::bad_address{};

    return std::move(addr);
}

inline
result<beast::http::response<beast::http::string_body>>
igd::soap_request( string_view command
                 , string_view message
                 , net::yield_context yield) noexcept
{
    using namespace std::chrono;
    namespace http = beast::http;

    auto host_port = _url.host_and_port();
    auto opt_remote_ep = str::consume_endpoint<net::ip::tcp>(host_port);
    if (!opt_remote_ep)
        return net::error::invalid_argument;

    std::string body =
        "<?xml version=\"1.0\" ?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>" + message.to_string() + "</s:Body>"
        "</s:Envelope>";

    http::request<http::string_body> rq{http::verb::post, _url, 11};
    rq.set(http::field::host, _url.host_and_port());
    rq.set(http::field::user_agent, CPP_UPNP_HTTP_USER_AGENT);
    rq.set(http::field::content_type, "text/xml; charset=\"utf-8\"");
    rq.set(http::field::connection, "Close");
    rq.set(http::field::cache_control, "no-cache");
    rq.set(http::field::pragma, "no-cache");
    rq.set("SOAPAction", _urn + "#" + command.to_string());

    rq.body() = std::move(body);
    rq.prepare_payload();

    //std::cerr << "----------------------------------------\n";
    //std::cerr << rq;

    sys::error_code ec;

    beast::tcp_stream stream(_exec);
    stream.expires_after(std::chrono::seconds(5));

    auto cancelled = _cancel.connect([&] { stream.close(); });

    stream.async_connect(*opt_remote_ep, yield[ec]);
    if (cancelled) return net::error::operation_aborted;
    if (ec) return ec;

    http::async_write(stream, rq, yield[ec]);
    if (cancelled) return net::error::operation_aborted;
    if (ec) return ec;

    beast::flat_buffer b;
    http::response<http::string_body> rs;

    http::async_read(stream, b, rs, yield[ec]);
    if (cancelled) return net::error::operation_aborted;
    if (ec) return ec;

    //std::cerr << rs;
    //std::cerr << "----------------------------------------\n";

    return std::move(rs);
}

/* static */
inline
result<std::vector<igd>> igd::discover(net::executor exec, net::yield_context yield)
{
    using namespace std;

    auto q = ssdp::query::start(exec, yield);
    if (!q) return q.error();

    auto qrr = q.value().get_response(yield);
    if (!qrr) return qrr.error();

    auto& qr = qrr.value();

    auto res_root_dev = query_root_device(exec, qr.location, yield);
    if (!res_root_dev) return sys::errc::io_error;
    auto& root_dev = res_root_dev.value();

    string v;

    if (root_dev.type == "urn:schemas-upnp-org:device:InternetGatewayDevice:1") {
        v = "1";
    } else
    if (root_dev.type == "urn:schemas-upnp-org:device:InternetGatewayDevice:2") {
        v = "2";
    } else {
        return sys::errc::io_error;
    }

    string device_urn     = "urn:schemas-upnp-org:device:WANDevice:"           + v;
    string connection_urn = "urn:schemas-upnp-org:device:WANConnectionDevice:" + v;
    string con_ip         = "urn:schemas-upnp-org:service:WANIPConnection:"    + v;
    string con_ppp        = "urn:schemas-upnp-org:service:WANPPPConnection:"   + v;

    std::vector<igd> igds;

    for (const auto& device : root_dev.devices) {
        if (device.type != device_urn) continue;

        for (const auto& connection : device.devices) {
            if (connection.type != connection_urn) continue;

            for (const auto& service : connection.services) {
                if (service.type != con_ip && service.type != con_ppp)continue;

                url_t url = qr.location;
                url.replace_path(service.control_url.path());

                igds.push_back({
                    qr.uuid,
                    device,
                    service.id,
                    url,
                    service.type,
                    exec
                });
            }
        }
    }

    return std::move(igds);
}

/* static */
inline
result<device>
igd::query_root_device( net::executor exec
                      , const url_t& url
                      , net::yield_context yield) noexcept
{
    namespace http = beast::http;
    using request  = http::request<http::empty_body>;
    using response = http::response<http::string_body>;

    error_code ec;
    net::ip::tcp::resolver resolver(exec);

    auto hp = url.host_and_port();
    auto ep = str::consume_endpoint<net::ip::tcp>(hp);

    if (!ep) return sys::errc::invalid_argument;

    beast::tcp_stream stream(exec);
    stream.expires_after(std::chrono::seconds(5));

    stream.async_connect(*ep, yield[ec]);
    if (ec) return ec;

    request rq{http::verb::get, url.path(), 11};

    rq.set(http::field::host, url.host_and_port());
    rq.set(http::field::user_agent, CPP_UPNP_HTTP_USER_AGENT);

    http::async_write(stream, rq, yield[ec]);
    if (ec) return ec;

    beast::flat_buffer b;
    response rs;

    http::async_read(stream, b, rs, yield[ec]);
    if (ec) return ec;

    if (rs.result() != beast::http::status::ok) {
        return sys::errc::protocol_error;
    }

    auto opt_root_dev = device::parse_root(rs.body());
    if (!opt_root_dev) return sys::errc::io_error;

    return std::move(*opt_root_dev);
}

inline
void igd::stop() {
    _cancel();
}

} // upnp namespace
