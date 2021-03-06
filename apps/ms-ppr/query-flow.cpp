/*
 * Copyright (c) 2015 Qin Liu.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 */

#include <vector>
#include <string>
#include <fstream>
#include <map>

#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <boost/container/flat_map.hpp>

#include <graphlab.hpp>

typedef float float_type;

// Global random reset probability
const float_type RESET_PROB = 0.15;

float_type threshold;
int niters;
bool no_index;
boost::unordered_set<graphlab::vertex_id_type> *sources = NULL;

enum phase_t {INIT_GRAPH, COMPUTE};
phase_t phase = INIT_GRAPH;

typedef boost::container::flat_map<graphlab::vertex_id_type, uint16_t> map_t;
typedef boost::container::flat_map<graphlab::vertex_id_type, float_type> vec_map_t;
typedef boost::unordered_map<graphlab::vertex_id_type, float_type> vec_map2_t;

template <typename T>
struct vec_type {
    typedef T map_type;

    T val;

    vec_type() : val() { }

    vec_type(T&& val) : val(std::move(val)) { }
    vec_type(const vec_type& other) : val(other.val) { }
    vec_type(vec_type&& other) : val(std::move(other.val)) { }

    inline void save(graphlab::oarchive& oarc) const {
        oarc << val;
    }

    inline void load(graphlab::iarchive& iarc) {
        iarc >> val;
    }

    inline bool empty() const {
        return val.empty();
    }

    inline void clear() {
        val.clear();
    }

    vec_type& operator+=(const vec_type& other) {
        for (auto it = other.val.begin(); it != other.val.end(); ++it) {
            val[it->first] += it->second;
        }
        return *this;
    }

    vec_type& operator=(const vec_type& other) {
        val = other.val;
        return *this;
    }

    vec_type& operator=(vec_type&& other) {
        val = std::move(other.val);
        return *this;
    }
};

typedef vec_type<vec_map_t> vec_t;
typedef vec_type<vec_map2_t> vec2_t;

struct VertexData {
    vec_t ppr, flow, residual;

    VertexData() : ppr(), flow(), residual() {}

    void save(graphlab::oarchive& oarc) const {
        if (phase == INIT_GRAPH) {
            map_t counter;
            for (auto it = ppr.val.begin(); it != ppr.val.end(); ++it)
                counter[it->first] = it->second * 0xFFFF;
            oarc << counter;
        } else {
            oarc << ppr << flow << residual;
        }
    }

    void load(graphlab::iarchive& iarc) {
        if (phase == INIT_GRAPH) {
            map_t counter;
            iarc >> counter;
            float_type sum = 0.0;
            for (auto it = counter.begin(); it != counter.end(); ++it)
                sum += it->second;
            vec_t::map_type val(counter.begin(), counter.end());
            for (auto it = val.begin(); it != val.end(); ++it)
                it->second /= sum;
            ppr.val = std::move(val);
        } else {
            iarc >> ppr >> flow >> residual;
        }
    }
};

typedef graphlab::empty EdgeData; // no edge data

// The graph type is determined by the vertex and edge data types
typedef graphlab::distributed_graph<VertexData, EdgeData> graph_type;

class DecompositionProgram : public graphlab::ivertex_program<graph_type,
    graphlab::empty, vec_t> {
private:
    message_type flow;

public:
    DecompositionProgram() : flow() {}

    void init(icontext_type& context, const vertex_type& vertex,
            const message_type& msg) {
        if (context.iteration() == 0) {
            if (sources == NULL || sources->find(vertex.id()) != sources->end())
                flow.val[vertex.id()] = 1.0;
        } else
            flow = std::move(msg);
    }

    edge_dir_type gather_edges(icontext_type& context,
            const vertex_type& vertex) const {
        return graphlab::NO_EDGES;
    }

    graphlab::empty gather(icontext_type& context, const vertex_type& vertex,
            edge_type& edge) const {
        return graphlab::empty();
    }

    void apply(icontext_type& context, vertex_type& vertex,
            const gather_type& total) {
        if (context.iteration() == niters-1) {
            vertex.data().flow += flow;
            flow.clear();
            return;
        }

        vec_t new_flow;
        if (!flow.empty()) {
            float_type c = (1-RESET_PROB) * (vertex.num_out_edges() > 0 ? 1.0 / vertex.num_out_edges() : 1.0);
            for (auto it = flow.val.begin(); it != flow.val.end(); ++it) {
                vertex.data().residual.val[it->first] += RESET_PROB * it->second;
                float_type t = c * it->second;
                if (t > threshold)
                    new_flow.val[it->first] = t;
            }
        }
        flow = std::move(new_flow);
    }

    edge_dir_type scatter_edges(icontext_type& context,
            const vertex_type& vertex) const {
        if (!flow.empty())
            return graphlab::OUT_EDGES;
        else
            return graphlab::NO_EDGES;
    }

    void scatter(icontext_type& context, const vertex_type& vertex,
            edge_type& edge) const {
        context.signal(edge.target(), vec_t(flow));
    }

    void save(graphlab::oarchive& oarc) const {
        oarc << flow;
    }

    void load(graphlab::iarchive& iarc) {
        iarc >> flow;
    }
};

class CollectProgram : public graphlab::ivertex_program<graph_type,
    graphlab::empty, vec2_t> {
private:
    message_type ppr;

public:
    void init(icontext_type& context, const vertex_type& vertex,
            const message_type& msg) {
        ppr = std::move(msg);
    }

    edge_dir_type gather_edges(icontext_type& context,
            const vertex_type& vertex) const {
        return graphlab::NO_EDGES;
    }

    graphlab::empty gather(icontext_type& context, const vertex_type& vertex,
            edge_type& edge) const {
        return graphlab::empty();
    }

    void apply(icontext_type& context, vertex_type& vertex,
            const gather_type& total) {
        if (!ppr.empty()) {
            vertex.data().ppr.val = vec_t::map_type(ppr.val.begin(), ppr.val.end());
        }
    }

    edge_dir_type scatter_edges(icontext_type& context,
            const vertex_type& vertex) const {
        return graphlab::NO_EDGES;
    }

    void scatter(icontext_type& context, const vertex_type& vertex,
            edge_type& edge) const {
    }

    void save(graphlab::oarchive& oarc) const { }

    void load(graphlab::iarchive& iarc) { }
};

typedef graphlab::synchronous_engine<CollectProgram> engine_type;
void collect_results(engine_type::icontext_type& context,
        graph_type::vertex_type& vertex) {
    if (!no_index) {
        for (auto it = vertex.data().flow.val.begin(); it !=
                vertex.data().flow.val.end(); ++it) {
            if (it->second < threshold)
                continue;
            engine_type::message_type::map_type val(
                    vertex.data().ppr.val.begin(),
                    vertex.data().ppr.val.end());
            for (auto it2 = val.begin(); it2 != val.end(); ++it2)
                it2->second *= it->second;
            auto it2 = vertex.data().residual.val.find(it->first);
            if (it2 != vertex.data().residual.val.end()) {
                val[vertex.id()] += it2->second;
                it2->second = 0.0;
            }
            engine_type::message_type msg(std::move(val));
            context.signal_vid(it->first, msg);
        }
    }
    for (auto it = vertex.data().residual.val.begin(); it !=
            vertex.data().residual.val.end(); ++it) {
        if (it->second < threshold)
            continue;
        engine_type::message_type msg;
        msg.val[vertex.id()] = it->second;
        context.signal_vid(it->first, msg);
    }
    vertex.data().residual.clear();
}

bool compare(const std::pair<graphlab::vertex_id_type, float_type>& firstElem,
        const std::pair<graphlab::vertex_id_type, float_type>& secondElem) {
      return firstElem.second > secondElem.second;
}

struct pagerank_writer {
    size_t topk;

    pagerank_writer(size_t topk) : topk(topk) { }

    std::string save_vertex(graph_type::vertex_type vertex) {
        std::stringstream strm;
        if (!vertex.data().ppr.empty()) {
            strm << vertex.id();
            std::vector<std::pair<graphlab::vertex_id_type, float_type> >
                result(vertex.data().ppr.val.begin(), vertex.data().ppr.val.end());
            std::sort(result.begin(), result.end(), compare);
            size_t len = std::min(topk, result.size());
            strm << " " << len;
            for (size_t i = 0; i < len; ++i)
                strm << " " << result[i].first;
            strm << std::endl;
        }
        return strm.str();
    }
    std::string save_edge(graph_type::edge_type e) { return ""; }
};

int main(int argc, char** argv) {
    // Initialize control plane using mpi
    graphlab::mpi_tools::init(argc, argv);
    graphlab::distributed_control dc;
    global_logger().set_log_level(LOG_INFO);

    // Parse command line options -----------------------------------------------
    graphlab::command_line_options clopts("Multi-Source "
            "Personalized PageRank algorithm.");
    std::string graph_dir;
    std::string format = "snap";
    clopts.attach_option("graph", graph_dir,
            "The binary graph file that contains preprocessed PPR."
            "Must be provided.");
    clopts.add_positional("graph");
    clopts.attach_option("format", format, "The graph file format");
    niters = 10;
    clopts.attach_option("niters", niters,
            "Number of iterations");
    threshold = 1e-4;
    clopts.attach_option("threshold", threshold,
            "The threshold of flow");
    std::string bin_prefix;
    clopts.attach_option("bin_prefix", bin_prefix,
            "If set, will save the whole graph to a sequence "
            "of binary files with prefix bin_prefix");
    std::string saveprefix;
    clopts.attach_option("saveprefix", saveprefix,
            "If set, will save the whole graph to a "
            "sequence of files with prefix saveprefix");
    size_t topk = 100;
    clopts.attach_option("topk", topk,
            "Output top-k elements of PPR vectors");
    std::string sources_file;
    clopts.attach_option("sources_file", sources_file,
            "The file contains all sources.");
    int max_num_sources = 1000;
    clopts.attach_option("num_sources", max_num_sources,
            "The number of sources");
    no_index = false;
    clopts.attach_option("no_index", no_index,
            "Compute PPR vectors without preprocessed index.");

    if(!clopts.parse(argc, argv)) {
        dc.cout() << "Error in parsing command line arguments." << std::endl;
        return EXIT_FAILURE;
    }

    clopts.get_engine_args().set_option("enable_sync_vertex_data", false);
    clopts.get_engine_args().set_option("max_iterations", ++niters);

    // Build the graph ----------------------------------------------------------
    double start_time = graphlab::timer::approx_time_seconds();
    phase = INIT_GRAPH;
    graph_type graph(dc, clopts);
    if (no_index) {
        dc.cout() << "Loading graph in format: "<< format << std::endl;
        graph.load_format(graph_dir, format);
    } else {
        dc.cout() << "Loading graph and index in binary" << std::endl;
        graph.load_binary(graph_dir);
    }
    // must call finalize before querying the graph
    graph.finalize();
    dc.cout() << "#vertices: " << graph.num_vertices()
        << " #edges:" << graph.num_edges() << std::endl;
    double runtime = graphlab::timer::approx_time_seconds() - start_time;
    dc.cout() << "loading : " << runtime << " seconds" << std::endl;

    if (sources_file.length() > 0) {
        sources = new boost::unordered_set<graphlab::vertex_id_type>();
        std::ifstream fin(sources_file.c_str());
        int num_sources;
        fin >> num_sources;
        for (int i = 0; i < std::min(num_sources, max_num_sources); ++i) {
            graphlab::vertex_id_type vid;
            fin >> vid;
            sources->insert(vid);
        }
    }

    // Running The Engine -------------------------------------------------------
    phase = COMPUTE;
    graphlab::synchronous_engine<DecompositionProgram> *engine = new
        graphlab::synchronous_engine<DecompositionProgram>(dc, graph, clopts);
    graphlab::timer timer;
    engine->signal_all();
    engine->start();
    dc.cout() << "decomposition : " << engine->elapsed_seconds() <<
        " seconds" << std::endl;
    delete engine;

    clopts.get_engine_args().set_option("max_iterations", 1);
    graphlab::synchronous_engine<CollectProgram> engine2(dc, graph, clopts);
    start_time = graphlab::timer::approx_time_seconds();
    engine2.transform_vertices(collect_results);
    engine2.start();
    runtime = graphlab::timer::approx_time_seconds() - start_time;
    dc.cout() << "sum-up : " << runtime << " seconds" << std::endl;

    if (sources)
        delete sources;

    dc.cout() << "runtime : " << timer.current_time() << " seconds" <<
        std::endl;

    // Save the final graph -----------------------------------------------------
    start_time = graphlab::timer::approx_time_seconds();
    if (bin_prefix != "") {
        phase = INIT_GRAPH;
        graph.save_binary(bin_prefix);
    }
    if (saveprefix != "") {
        graph.save(saveprefix, pagerank_writer(topk),
                false,    // do not gzip
                true,     // save vertices
                false);   // do not save edges
    }
    runtime = graphlab::timer::approx_time_seconds() - start_time;
    dc.cout() << "save : " << runtime << " seconds" << std::endl;

    // Tear-down communication layer and quit -----------------------------------
    graphlab::mpi_tools::finalize();
    return EXIT_SUCCESS;
}

