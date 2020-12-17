/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020  David Shah <dave@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#ifdef USE_OPENCL

#include "ocular.h"
#include "nextpnr.h"
#include "opencl.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

/*

OCuLaR - Open Computing Language Router

This is a GPGPU router inspired by Corolla [1] with modifications to make it more
suited to the APIs and environment that nextpnr provides. Much of the technique
detail is based on [2].

[1] Corolla: GPU-Accelerated FPGA Routing Based onSubgraph Dynamic Expansion
Minghua Shen, Guojie Luo
https://ceca.pku.edu.cn/media/lw/137e5df7dec627f988e07d54ff222857.pdf

[2] Work-Efficient Parallel GPU Methods for Single-Source Shortest Paths
Andrew Davidson, Sean Baxter, Michael Garland, John D. Owens
https://escholarship.org/uc/item/8qr166v2
*/

struct OcularRouter
{
    Context *ctx;
    std::unique_ptr<cl::Context> clctx;
    std::unique_ptr<cl::Program> clprog;

    // Some magic constants
    const float delay_scale = 1000.0f; // conversion from float ns to int ps

    const int32_t inf_cost = 0x7FFFFFF;

    /*
        GPU-side routing graph

        At the moment this is a simple flattened graph. Longer term, ways of
        deduplicating this without excessive startup effort or excessively
        complex GPU-side code should be investigated. This might have to be
        once we have shared-between-arches deduplication cracked in general.

        Because we currently only do forward routing in the GPU, this graph
        only needs to be linked in one direction

        Costs in the graph are currently converted to int32s, to enable use
        of atomic updates to improve determinism
    */
    // Wire locations for bounding box tests
    BackedGPUBuffer<int16_t> wire_x, wire_y;
    // Pointer to start in adjaency list -- by wire index
    BackedGPUBuffer<uint32_t> adj_offset;
    // Adjacency list entries -- downhill wire index and cost
    BackedGPUBuffer<uint32_t> edge_dst_index;
    // PIP costs - these will be increased as time goes on
    // to account for historical congestion
    BackedGPUBuffer<int32_t> edge_cost;
    // The GPU doesn't care about these, but we need to corrolate between
    // an adjacency list index and a concrete PIP when we bind the GPU's
    // result
    std::vector<PipId> edge_pip;

    // Some non-GPU fields that are kept in sync with the GPU wire indices
    struct PerWireData
    {
        WireId w;
    };
    std::vector<PerWireData> wire_data;
    std::unordered_map<WireId, int32_t> wire_to_index;

    int width = 0, height = 0;

    // Similar non-GPU related net data
    struct PerNetData
    {
        NetInfo *ni;
        ArcBounds bb;
        bool undriven;
        bool fixed_routing;
    };

    /*
        Current routing state. We need to maintain the following:
           - current cost of a node, or 'infinity' if it hasn't been visited yet
           - the adjacency list entry (that can be corrolated to a pip) used to reach a node
           - current 'near' queue that nodes/edges are being worked on (per workgroup)
           - next 'near' queue that nearby nodes to explore are added to (per workgroup)
           - next 'far' queue that far-away nodes to explore are added to (per workgroup)
           - current newly-dirtied nodes that will need their costs reset to 'infinity' once the current net is routed
       (per workgroup)
           - number of unique nets bound to a node, to determine congestion-related costs
    */
    BackedGPUBuffer<int32_t> current_cost;
    GPUBuffer<uint32_t> uphill_edge;
    // To avoid copies, we swap 'A' and 'B' between current/next queues at every iteration
    GPUBuffer<uint32_t> near_queue_a, near_queue_b;
    // For the next, added-to queue, this is a count starting from 0 for each group
    // For the current, worked-from queue, this is a prefix sum so we can do a binary search to find work
    BackedGPUBuffer<uint32_t> near_queue_count_a, near_queue_count_b;
    // We don't have A/B for the far queue, because it is never directly worked from
    GPUBuffer<uint32_t> far_queue, far_queue_count;

    GPUBuffer<uint32_t> dirtied_nodes, dirtied_nodes_count;
    BackedGPUBuffer<uint8_t> bound_count;

    /*
        Current routing configuration
        This structure is per in-flight net
    */
    NPNR_PACKED_STRUCT(struct NetConfig {
        // Net bounding box
        cl_short x0, y0, x1, y1;
        // max size of the near and far queue
        cl_int near_queue_size, far_queue_size;
        // max size of the dirtied nodes structure
        cl_int dirtied_nodes_size;
        // start and end workgroup offsets for the net
        cl_int net_start, net_end;
        // current congestion cost
        cl_float curr_cong_cost;
        // near/far threshold
        cl_int near_far_thresh;
        // number of nodes to process per workgroup
        cl_int group_nodes;
    });

    /*
        Purely host-side per-inflight-net configuration
    */
    struct InFlightNet
    {
        // index into the flat list of nets, or -1 if this slot isn't used
        int net_idx = -1;
        // ...
    };

    // CPU side grid->net map, so we don't route overlapping nets at once
    std::vector<int8_t> grid2net;

    /*
        Workgroup configuration
    */
    NPNR_PACKED_STRUCT(struct WorkgroupConfig {
        cl_int net;
        cl_uint size;
    });

    // Route config per in-flight net
    BackedGPUBuffer<NetConfig> route_config;
    std::vector<InFlightNet> net_slots;

    BackedGPUBuffer<WorkgroupConfig> wg_config;

    OcularRouter(Context *ctx)
            : ctx(ctx), clctx(get_opencl_ctx(ctx)), clprog(get_opencl_program(*clctx, "ocular")),
              wire_x(*clctx, CL_MEM_READ_ONLY), wire_y(*clctx, CL_MEM_READ_ONLY), adj_offset(*clctx, CL_MEM_READ_ONLY),
              edge_dst_index(*clctx, CL_MEM_READ_ONLY), edge_cost(*clctx, CL_MEM_READ_WRITE),
              current_cost(*clctx, CL_MEM_READ_WRITE), uphill_edge(*clctx, CL_MEM_READ_WRITE),
              near_queue_a(*clctx, CL_MEM_READ_WRITE), near_queue_b(*clctx, CL_MEM_READ_WRITE),
              near_queue_count_a(*clctx, CL_MEM_READ_WRITE), near_queue_count_b(*clctx, CL_MEM_READ_WRITE),
              far_queue(*clctx, CL_MEM_READ_WRITE), far_queue_count(*clctx, CL_MEM_READ_WRITE),
              dirtied_nodes(*clctx, CL_MEM_READ_WRITE), dirtied_nodes_count(*clctx, CL_MEM_READ_WRITE),
              bound_count(*clctx, CL_MEM_READ_WRITE), route_config(*clctx, CL_MEM_READ_ONLY),
              wg_config(*clctx, CL_MEM_READ_ONLY)
    {
    }

    void build_graph()
    {
        log_info("Importing routing graph...\n");
        // Build the GPU-oriented, flattened routing graph from the Arch-provided data
        for (auto wire : ctx->getWires()) {
            // Get the centroid of the wire for hit-testing purposes
            ArcBounds wire_loc = ctx->getRouteBoundingBox(wire, wire);
            short cx = (wire_loc.x0 + wire_loc.x1) / 2;
            short cy = (wire_loc.y0 + wire_loc.y1) / 2;

            wire_x.push_back(cx);
            wire_y.push_back(cy);

            PerWireData wd;
            wd.w = wire;
            wire_to_index[wire] = int(wire_data.size());
            wire_data.push_back(wd);

            width = std::max<int>(wire_loc.x1 + 1, width);
            height = std::max<int>(wire_loc.y1 + 1, height);
        }

        // Construct the CSR format adjacency list
        adj_offset.resize(wire_data.size() + 1);

        for (size_t i = 0; i < wire_data.size(); i++) {
            WireId w = wire_data.at(i).w;
            // CSR offset
            adj_offset.at(i) = edge_dst_index.size();
            for (PipId p : ctx->getPipsDownhill(w)) {
                // Ignore permanently unavailable pips, and pips bound before we enter the router (e.g. for gclks)
                if (!ctx->checkPipAvail(p))
                    continue;
                WireId dst = ctx->getPipDstWire(p);
                if (!ctx->checkWireAvail(dst))
                    continue;
                // Compute integer cost; combined cost of the pip and the wire it drives
                int base_cost = int((ctx->getDelayNS(ctx->getPipDelay(p).maxDelay()) +
                                     ctx->getDelayNS(ctx->getWireDelay(dst).maxDelay())) *
                                    delay_scale);
                // Add to the adjacency list
                edge_cost.push_back(base_cost);
                edge_dst_index.push_back(wire_to_index.at(dst));
                edge_pip.push_back(p);
            }
        }
        // Final offset so we know the total size of the list; for the last node
        adj_offset.at(wire_data.size()) = edge_dst_index.size();

        // Resize some other per-net structures
        current_cost.resize(wire_data.size());
        std::fill(current_cost.begin(), current_cost.end(), inf_cost);
        uphill_edge.resize(wire_data.size());
        bound_count.resize(wire_data.size());
    }

    void import_nets()
    {
        log_info("Importing nets...\n");
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            PerNetData nd;
            nd.ni = ni;
            // Initial bounding box is the null space
            nd.bb.x0 = ctx->getGridDimX() - 1;
            nd.bb.y0 = ctx->getGridDimY() - 1;
            nd.bb.x1 = 0;
            nd.bb.y1 = 0;
            if (ni->driver.cell != nullptr) {
                nd.bb.extend(ctx->getBelLocation(ni->driver.cell->bel));
            } else {
                nd.undriven = true;
            }
            for (auto &usr : ni->users) {
                nd.bb.extend(ctx->getBelLocation(usr.cell->bel));
            }
            nd.fixed_routing = false;
            // Check for existing routing (e.g. global clocks routed earlier)
            if (!ni->wires.empty()) {
                bool invalid_route = false;
                for (auto &usr : ni->users) {
                    WireId wire = ctx->getNetinfoSinkWire(ni, usr);
                    if (!ni->wires.count(wire))
                        invalid_route = true;
                    else if (ni->wires.at(wire).strength > STRENGTH_STRONG)
                        nd.fixed_routing = true;
                }
                if (nd.fixed_routing) {
                    if (invalid_route)
                        log_error("Combination of locked and incomplete routing on net '%s' is unsupported.\n",
                                  ctx->nameOf(ni));
                    // Mark wires as used so they have a congestion penalty associated with them
                    for (auto &wire : ni->wires) {
                        int idx = wire_to_index.at(wire.first);
                        NPNR_ASSERT(bound_count.at(idx) == 0); // no overlaps allowed for locked routing
                        bound_count.at(idx)++;
                    }
                } else {
                    // Routing isn't fixed, just rip it up so we don't worry about it
                    ctx->ripupNet(ni->name);
                }
            }
        }
    }

    // Work partitioning and queue configuration - TODO: make these dynamic
    const int num_workgroups = 64;
    const int near_queue_len = 15000;
    const int far_queue_len = 100000;
    const int dirty_queue_len = 100000;
    const int workgroup_size = 128;
    const int max_nets_in_flight = 32;

    void alloc_buffers()
    {
        // Near queues (two because we swap them)
        near_queue_a.resize(near_queue_len * num_workgroups);
        near_queue_count_a.resize(num_workgroups);
        near_queue_b.resize(near_queue_len * num_workgroups);
        near_queue_count_b.resize(num_workgroups);
        // Far queue
        far_queue.resize(far_queue_len * num_workgroups);
        far_queue_count.resize(num_workgroups);
        // Per-workgroup dirty node list
        dirtied_nodes.resize(dirty_queue_len * num_workgroups);
        dirtied_nodes_count.resize(num_workgroups);

        route_config.resize(max_nets_in_flight);
        net_slots.resize(max_nets_in_flight);
        wg_config.resize(workgroup_size);
        for (auto &wg : wg_config)
            wg.size = workgroup_size;

        grid2net.resize(width * height);
    }

    void mark_region(int x0, int y0, int x1, int y1, int8_t value)
    {
        for (int y = y0; y <= y1; y++) {
            NPNR_ASSERT(y >= 0 && y < height);
            for (int x = x0; x <= x1; x++) {
                NPNR_ASSERT(x >= 0 && x < width);
                grid2net[y * width + x] = value;
            }
        }
    }

    bool check_region(int x0, int y0, int x1, int y1, int8_t value = -1)
    {
        for (int y = y0; y <= y1; y++) {
            NPNR_ASSERT(y >= 0 && y < height);
            for (int x = x0; x <= x1; x++) {
                NPNR_ASSERT(x >= 0 && x < width);
                if (grid2net[y * width + x] != value)
                    return false;
            }
        }
        return true;
    }

    template <typename T> T prefix_sum(const BackedGPUBuffer<T> &in, int count)
    {
        T sum = 0;
        for (int i = 0; i < count; i++) {
            sum += in.at(i);
            in.at(i) = sum;
        }
        return sum;
    }

    bool operator()()
    {
        // The sequence of things to do
        build_graph();
        import_nets();
        alloc_buffers();
        return true;
    }
};

bool router_ocular(Context *ctx)
{
    OcularRouter router(ctx);
    return router();
}

NEXTPNR_NAMESPACE_END

#endif