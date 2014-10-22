#ifndef PLAIN_DESCRIPTOR_H
#define PLAIN_DESCRIPTOR_H

#include "BaseDescriptor.h"

template <class DataFacadeT> class PLAINDescriptor final : public BaseDescriptor<DataFacadeT>
{
  private:
    DescriptorConfig config;
    FixedPointCoordinate current;
    DataFacadeT *facade;

    void AddRoutePoint(const FixedPointCoordinate &coordinate, std::vector<char> &output)
    {
        const std::string route_point_head = "<rtept lat=\"";
        const std::string route_point_middle = " lon=\"";
        const std::string route_point_tail = "\"></rtept>";

        std::string tmp;

        FixedPointCoordinate::convertInternalLatLonToString(coordinate.lat, tmp);
        output.insert(output.end(), route_point_head.begin(), route_point_head.end());
        output.insert(output.end(), tmp.begin(), tmp.end());
        output.push_back('\"');

        FixedPointCoordinate::convertInternalLatLonToString(coordinate.lon, tmp);
        output.insert(output.end(), route_point_middle.begin(), route_point_middle.end());
        output.insert(output.end(), tmp.begin(), tmp.end());
        output.insert(output.end(), route_point_tail.begin(), route_point_tail.end());
    }

  public:
    explicit PLAINDescriptor(DataFacadeT *facade) : facade(facade) {}

    void SetConfig(const DescriptorConfig &c) final { config = c; }

    // TODO: reorder parameters
    void Run(const RawRouteData &raw_route, http::Reply &reply) final
    {
        std::string header("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                           "<gpx creator=\"PLAIN OSRM Routing Engine\" version=\"1.1\" "
                           "xmlns=\"http://www.topografix.com/GPX/1/1\" "
                           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                           "xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 gpx.xsd"
                           "\">"
                           "<metadata><copyright author=\"Project OSRM\"><license>Data (c)"
                           " OpenStreetMap contributors (ODbL)</license></copyright>"
                           "</metadata>"
                           "<rte>");
        reply.content.insert(reply.content.end(), header.begin(), header.end());
        const bool found_route = (raw_route.shortest_path_length != INVALID_EDGE_WEIGHT) &&
                                 (!raw_route.unpacked_path_segments.front().empty());
        if (found_route)
        {
            AddRoutePoint(raw_route.segment_end_coordinates.front().source_phantom.location,
                          reply.content);

            for (const std::vector<PathData> &path_data_vector : raw_route.unpacked_path_segments)
            {
                for (const PathData &path_data : path_data_vector)
                {
                    const FixedPointCoordinate current_coordinate =
                        facade->GetCoordinateOfNode(path_data.node);
                    AddRoutePoint(current_coordinate, reply.content);
                }
            }
            AddRoutePoint(raw_route.segment_end_coordinates.back().target_phantom.location,
                          reply.content);
        }
        std::string footer("</rte></gpx>");
        reply.content.insert(reply.content.end(), footer.begin(), footer.end());
    }
};
#endif // PLAIN_DESCRIPTOR_H
