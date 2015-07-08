#include <vanetza/dcc/data_request.hpp>
#include <vanetza/dcc/interface.hpp>
#include <vanetza/geonet/areas.hpp>
#include <vanetza/geonet/data_confirm.hpp>
#include <vanetza/geonet/data_indication.hpp>
#include <vanetza/geonet/mib.hpp>
#include <vanetza/geonet/packet.hpp>
#include <vanetza/geonet/router.hpp>
#include <vanetza/geonet/timestamp.hpp>
#include <vanetza/geonet/tests/network_topology.hpp>
#include <vanetza/net/mac_address.hpp>
#include <boost/optional.hpp>
#include <GeographicLib/Geocentric.hpp>
#include <GeographicLib/Geodesic.hpp>
#include <GeographicLib/LocalCartesian.hpp>
#include <list>
#include <unordered_map>

using namespace vanetza;
using namespace vanetza::geonet;
using namespace vanetza::dcc;
using namespace vanetza::units::si;

NetworkTopology::RequestInterface::RequestInterface(NetworkTopology& network, const MacAddress& mac) :
    m_network(network), m_address(mac)
{
}

void NetworkTopology::RequestInterface::request(const dcc::DataRequest& req, std::unique_ptr<ChunkPacket> packet)
{
    m_last_request = req;
    m_last_packet.reset(new ChunkPacket(*packet));
    m_last_request.source = m_address;
    m_network.save_request(m_last_request, std::move(packet));
}

NetworkTopology::RouterContext::RouterContext(NetworkTopology& network) :
    request_interface(network, mac_address),
    router(network.get_mib(), request_interface)
{
}

boost::optional<NetworkTopology::RouterContext&> NetworkTopology::get_host(const MacAddress& addr)
{
    boost::optional<RouterContext&> context;
    auto found = hosts.find(addr);
    if (found != hosts.end())
        context = *found->second;

    return context;
}

boost::optional<Router&> NetworkTopology::get_router(const MacAddress& addr)
{
    boost::optional<Router&> router;
    auto context = get_host(addr);
    if (context)
        router = context->router;

    return router;
}

boost::optional<NetworkTopology::RequestInterface&> NetworkTopology::get_interface(const MacAddress& addr)
{
    boost::optional<NetworkTopology::RequestInterface&> interface;
    auto context = get_host(addr);
    if (context)
        interface = context->request_interface;

    return interface;
}

const unsigned& NetworkTopology::get_counter_requests(const MacAddress& addr)
{
    return counter_requests[addr];
}

void NetworkTopology::add_router(const MacAddress& addr)
{
    std::unique_ptr<RouterContext> context { new RouterContext(*this) };
    context->mac_address = addr;
    context->router.set_address(Address(context->mac_address));
    context->router.set_time(now);

    hosts.emplace(addr, std::move(context));
}

void NetworkTopology::add_reachability(const MacAddress& addr, std::list<MacAddress> reachables)
{
    // save reachable routers (MacAddresses) in map reachability
    reachability.emplace(addr, reachables);
}

void NetworkTopology::save_request(const dcc::DataRequest& req, std::unique_ptr<ChunkPacket> packet)
{
    // save request with packet in list requests
    requests.emplace_back(req, std::move(packet));

    // increment request counter
    counter_requests[req.source]++;
}

void NetworkTopology::dispatch()
{
    // process a stable sequence of saved requests
    decltype(requests) current_requests;
    std::swap(current_requests, requests);
    for (auto& tuple: current_requests) {
        // extract request and packet from tuple
        auto req = std::get<0>(tuple);
        get_interface(req.source)->m_last_packet.reset(new ChunkPacket(*std::get<1>(tuple)));

        // broadcast packet to all reachable routers
        if(req.destination == cBroadcastMacAddress) {
            // extract list with reachable MacAddresses
            auto recipients = reachability[req.source];
            // iterate through list of reachable MacAddresses
            for(auto& mac_addy: recipients) {
                // find mapped router and indicate
                send(req.source, mac_addy);
            }
        }
        // send packet only to router according to req.destination
        else {
            // find mapped router and indicate
            send(req.source, req.destination);
        }
    }
}

void NetworkTopology::send(const MacAddress& sender, const MacAddress& destination)
{
    counter_indications++;
    std::unique_ptr<UpPacket> packet_up { new UpPacket(*get_interface(sender)->m_last_packet) };
    auto router = get_router(destination);
    if (router) router->indicate(std::move(packet_up), sender, destination);
}

void NetworkTopology::set_position(const MacAddress& addr, CartesianPosition c)
{
    // convert cartesian to geodetic position
    GeodeticPosition pos = convert_cartesian_geodetic(c);
    auto router = get_router(addr);
    if (router) {
        LongPositionVector lpv = router->get_local_position_vector();
        lpv.timestamp = now;
        lpv.latitude = geo_angle_i32t(pos.latitude);
        lpv.longitude = geo_angle_i32t(pos.longitude);
        router->update(lpv);
    }
}

void NetworkTopology::advance_time(Timestamp::duration_type t)
{
    const Timestamp::duration_type max_step { 500 * Timestamp::millisecond };
    do {
        const auto step = std::min(t, max_step);
        now += step;
        t -= step;
        // update timestamp for every router
        for (auto& host : hosts) {
            Router& router = host.second->router;
            router.update(now);
            LongPositionVector lpv = router.get_local_position_vector();
            lpv.timestamp = now;
            router.update(lpv);
        }
       dispatch();
    } while (t.value() > 0);
}

void NetworkTopology::reset_counters()
{
    counter_indications = 0;
    counter_requests.clear();
}

GeodeticPosition convert_cartesian_geodetic(const CartesianPosition& cart)
{
    const GeographicLib::Geocentric& earth = GeographicLib::Geocentric::WGS84();
    double lat = 0.0, lon = 0.0, unused_h = 0.0;
    GeographicLib::LocalCartesian proj(lat, lon, unused_h, earth);
    double x = cart.x / meter;
    double y = cart.y / meter;
    double unused_z = 0.0;
    proj.Reverse(x, y, unused_z, lat, lon, unused_h);

    return GeodeticPosition(lat * vanetza::units::degree, lon * vanetza::units::degree);
}

Area circle_dest_area(double radius, double midpoint_x, double midpoint_y)
{
    // create a round dest-area with delivered radius and midpoint
    Area dest_area;
    Circle c;
    c.r = radius * meter;
    dest_area.shape = c;
    dest_area.angle = vanetza::units::Angle(0.0 * vanetza::units::degree);
    dest_area.position = convert_cartesian_geodetic(CartesianPosition(midpoint_x * meter, midpoint_y * meter));

    return dest_area;
}