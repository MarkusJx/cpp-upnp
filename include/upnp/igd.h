#pragma once

#include <upnp/detail/namespaces.h>
#include <upnp/detail/cancel.h>
#include <upnp/core/result.h>
#include <upnp/core/string_view.h>
#include <upnp/core/variant.h>
#include <upnp/core/beast.h>
#include <upnp/device.h>
#include <boost/asio/spawn.hpp>

namespace upnp {

// Internet Gateway Device
class igd final {
private:
    using os_t = std::ostream;


public:
    enum protocol { tcp, udp };

    friend os_t& operator<<(os_t& os, const protocol& p) {
        return os << (p == tcp ? "TCP" : "UDP");
    }

public:
    struct error {
        struct aborted {};
        struct igd_host_parse_failed {};
        struct soap_request {};
        struct no_endpoint_to_igd {};
        struct bad_response_status {
            beast::http::status status;
        };
        struct invalid_xml_body {};
        struct bad_result {};
        struct bad_address {};

        friend os_t& operator<<(os_t& os, const aborted&) {
            return os << "operation aborted";
        }
        friend os_t& operator<<(os_t& os, const igd_host_parse_failed&) {
            return os << "failed to parse IGD host";
        }
        friend os_t& operator<<(os_t& os, const soap_request&) {
            return os << "failed to do soap request";
        }
        friend os_t& operator<<(os_t& os, const no_endpoint_to_igd&) {
            return os << "no suitable endpoint to IGD";
        }
        friend os_t& operator<<(os_t& os, const bad_response_status& e) {
            return os << "IGD resonded with non OK status " << e.status;
        }
        friend os_t& operator<<(os_t& os, const invalid_xml_body&) {
            return os << "failed to parse xml body";
        }
        friend os_t& operator<<(os_t& os, const bad_result&) {
            return os << "bad result";
        }
        friend os_t& operator<<(os_t& os, const bad_address&) {
            return os << "bad address";
        }

        using add_port_mapping = variant<
            aborted,
            igd_host_parse_failed,
            soap_request,
            no_endpoint_to_igd,
            bad_response_status
        >;

        using get_external_address = variant<
            soap_request,
            bad_response_status,
            invalid_xml_body,
            bad_result,
            bad_address
        >;
    };

public:
    igd(igd&&)            = default;
    igd& operator=(igd&&) = default;

    static
    result<std::vector<igd>> discover(net::executor, net::yield_context);

    /*
     *
     * This text https://tools.ietf.org/html/rfc6886#section-9.5 states that
     * setting @duration to != 0 may be a bad idea, although there seem to be
     * projects that use non zero values as a default and fall back to zero
     * (meaning maximum) if that fails. e.g.
     *
     * https://github.com/syncthing/syncthing/blob/119d76d0/lib/upnp/igd_service.go#L75-L77
     *
     */
    result< void
          , error::add_port_mapping
          >
    add_port_mapping( protocol
                    , uint16_t external_port
                    , uint16_t internal_port
                    , string_view description
                    , std::chrono::seconds duration
                    , net::yield_context yield) noexcept;

    result< net::ip::address
          , error::get_external_address
          >
    get_external_address(net::yield_context yield) noexcept;

    void stop();

    ~igd() { stop(); }

private:
    igd( std::string   uuid
       , device        upnp_device
       , std::string   service_id
       , url_t         url
       , std::string   urn
       , net::executor exec);

    static
    result<device>
    query_root_device(net::executor, const url_t&, net::yield_context) noexcept;

    result<beast::http::response<beast::http::string_body>>
    soap_request( string_view command
                , string_view message
                , net::yield_context) noexcept;

private:
    std::string   _uuid;
    device        _upnp_device;
    std::string   _service_id;
    url_t         _url;
    std::string   _urn;
    net::executor _exec;
    cancel_t      _cancel;
};

} // namespace upnp

#include <upnp/impl/igd.ipp>
