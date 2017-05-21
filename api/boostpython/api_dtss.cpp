#include "boostpython_pch.h"
#include <boost/python/docstring_options.hpp>

//-- for serialization:
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

//-- notice that boost serialization require us to
//   include shared_ptr/vector .. etc.. wherever it's needed

#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/vector.hpp>

//-- for the server
#include <dlib/server.h>
#include <dlib/iosockstream.h>

#include "api/time_series.h"
#include "core/dtss.h"
#include "dlib/timer.h"
// also consider policy: from https://www.codevate.com/blog/7-concurrency-with-embedded-python-in-a-multi-threaded-c-application

struct scoped_gil_release {
    scoped_gil_release() noexcept {
        py_thread_state = PyEval_SaveThread();
    }
    ~scoped_gil_release() noexcept {
        PyEval_RestoreThread(py_thread_state);
    }
    scoped_gil_release(const scoped_gil_release&) = delete;
    scoped_gil_release(scoped_gil_release&&) = delete;
    scoped_gil_release& operator=(const scoped_gil_release&) = delete;
private:
    PyThreadState * py_thread_state;
};

struct scoped_gil_aquire {
    scoped_gil_aquire() noexcept {
        py_state = PyGILState_Ensure();
    }
    ~scoped_gil_aquire() noexcept {
        PyGILState_Release(py_state);
    }
    scoped_gil_aquire(const scoped_gil_aquire&) = delete;
    scoped_gil_aquire(scoped_gil_aquire&&) = delete;
    scoped_gil_aquire& operator=(const scoped_gil_aquire&) = delete;
private:
    PyGILState_STATE   py_state;
};

namespace shyft {
    namespace dtss {

        struct py_server : server {
            boost::python::object cb;
            py_server():server([=](id_vector_t const &ts_ids,core::utcperiod p){return this->fire_cb(ts_ids,p); }) {
                if (!PyEval_ThreadsInitialized()) {
                    //std::cout << "InitThreads needed\n";
                    PyEval_InitThreads();// ensure threads-is enabled
                }
            }
            ~py_server() {
                //std::cout << "~dtss()\n";
                cb = boost::python::object();
            }


            static int msg_count ;

            ts_vector_t fire_cb(id_vector_t const &ts_ids,core::utcperiod p) {
                //std::cout << "cb("<<ts_ids.size()<<")\n";
                api::ats_vector r;
                if (cb.ptr()!=Py_None) {
                    scoped_gil_aquire gil;
                    //std::cout<<" py cb.."<<std::endl;std::cout.flush();
                    r = boost::python::call<ts_vector_t>(cb.ptr(), ts_ids, p);
                } else {
                    // for testing, just fill in constant values.
                    api::gta_t ta(p.start, core::deltahours(1), p.timespan() / core::deltahours(1));
                    for (size_t i = 0;i < ts_ids.size();++i)
                        r.push_back(api::apoint_ts(ta, double(i), time_series::ts_point_fx::POINT_AVERAGE_VALUE));
                }
                return r;
            }
            void process_messages(int msec) {
                scoped_gil_release gil;
                if(!is_running()) start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(msec));
            }
        };
        int py_server::msg_count = 0;
        // need to wrap core client to unlock gil during processing
        struct py_client {
            client impl;
            py_client(std::string host_port):impl(host_port) {}
            ~py_client() {
                //std::cout << "~py_client" << std::endl;
            }
            py_client(py_client const&) = delete;
            py_client(py_client &&) = delete;
            py_client& operator=(py_client const&o) = delete;

            void close(int timeout_ms=1000) {
                scoped_gil_release gil;
                impl.close(timeout_ms);
            }
            ts_vector_t percentiles(ts_vector_t const& tsv, core::utcperiod p,api::gta_t const&ta,std::vector<int> percentile_spec) {
                scoped_gil_release gil;
                return impl.percentiles(tsv,p,ta,percentile_spec);
            }
            ts_vector_t evaluate(ts_vector_t const& tsv, core::utcperiod p) {
                scoped_gil_release gil;
                return ts_vector_t(impl.evaluate(tsv,p));
            }

        };
    }
}


namespace expose {

    using namespace boost::python;
    void dtss_finalize() {
#ifdef _WIN32
        //to avoid infinite hang at exit on win, you need git clone https://github.com/sigbjorn/dlib
        WSACleanup();
#endif
    }
    static void dtss_messages() {
        def("dtss_finalize", dtss_finalize, "dlib socket and timer cleanup before exit python(automatically called once at module exit)");
    }
    static void dtss_server() {
        typedef shyft::dtss::py_server DtsServer;
        class_<DtsServer, boost::noncopyable >("DtsServer",
            doc_intro("A distributed time-series server object")
            doc_intro("Capable of processing time-series messages and responding accordingly")
            doc_intro("The user can setup callback to python to handle unbound symbolic time-series references")
            doc_intro("- that typically involve reading time-series from a service or storage for the specified period")
            doc_intro("The server object will then compute the resulting time-series vector,")
            doc_intro("and respond back to clients with the results")
            doc_see_also("shyft.api.DtsClient")
            )
            .def("set_listening_port", &DtsServer::set_listening_port, args("port_no"),
                doc_intro("set the listening port for the service")
                doc_parameters()
                doc_parameter("port_no","int","a valid and available tcp-ip port number to listen on.")
                doc_paramcont("typically it could be 20000 (avoid using official reserved numbers)")
                doc_returns("nothing","None","")
            )
            .def("start_async",&DtsServer::start_async,
                doc_intro("start server listening in background, and processing messages")
                doc_see_also("set_listening_port(port_no),is_running,cb,process_messages(msec)")
                doc_notes()
                doc_note("you should have setup up the callback, cb before calling start_async")
                doc_note("Also notice that processing will acquire the GIL\n -so you need to release the GIL to allow for processing messages")
                doc_see_also("process_messages(msec)")
            )
            .def("set_max_connections",&DtsServer::set_max_connections,args("max_connect"),
                doc_intro("limits simultaneous connections to the server (it's multithreaded!)")
                doc_parameters()
                doc_parameter("max_connect","int","maximum number of connections before denying more connections")
                doc_see_also("get_max_connections()")
            )
            .def("get_max_connections",&DtsServer::get_max_connections,"tbd")
            .def("clear",&DtsServer::clear,
                doc_intro("stop serving connections, gracefully.")
                doc_see_also("cb, process_messages(msec),start_async()")
            )
            .def("is_running",&DtsServer::is_running,
                doc_intro("true if server is listening and running")
                doc_see_also("start_async(),process_messages(msec)")
            )
            .def("get_listening_port",&DtsServer::get_listening_port,"returns the port number it's listening at")
            .def_readwrite("cb",&DtsServer::cb,
                doc_intro("callback for binding unresolved time-series references to concrete time-series.")
                doc_intro("Called *if* the incoming messages contains unbound time-series.")
                doc_intro("The signature of the callback function should be TsVector cb(StringVector,utcperiod)")
                doc_intro("\nExamples\n--------\n")
                doc_intro(
                    "from shyft import api as sa\n\n"
                    "def resolve_and_read_ts(ts_ids,read_period):\n"
                    "    print('ts_ids:', len(ts_ids), ', read period=', str(read_period))\n"
                    "    ta = sa.TimeAxis(read_period.start, sa.deltahours(1), read_period.timespan()//sa.deltahours(1))\n"
                    "    x_value = 1.0\n"
                    "    r = sa.TsVector()\n"
                    "    for ts_id in ts_ids :\n"
                    "        r.append(sa.TimeSeries(ta, fill_value = x_value))\n"
                    "        x_value = x_value + 1\n"
                    "    return r\n"
                    "# and then bind the function to the callback\n"
                    "dtss=sa.DtsServer()\n"
                    "dtss.cb=resolve_and_read_ts\n"
                    "dtss.set_listening_port(20000)\n"
                    "dtss.process_messages(60000)\n"
                )
            )
            .def("fire_cb",&DtsServer::fire_cb,args("msg","rp"),"testing fire from c++")
            .def("process_messages",&DtsServer::process_messages,args("msec"),
                doc_intro("wait and process messages for specified number of msec before returning")
                doc_intro("the dtss-server is started if not already running")
                doc_parameters()
                doc_parameter("msec","int","number of millisecond to process messages")
                doc_notes()
                doc_note("this method releases GIL so that callbacks are not blocked when the\n"
                    "dtss-threads perform the callback ")
                doc_see_also("cb,start_async(),is_running,clear()")
            )
            //.add_static_property("msg_count",
            //                     make_getter(&DtsServer::msg_count),
            //                     make_setter(&DtsServer::msg_count),"total number of requests")
            ;

    }
    static void dtss_client() {
        typedef shyft::dtss::py_client  DtsClient;
        class_<DtsClient, boost::noncopyable >("DtsClient",
            doc_intro("The client part of the DtsServer")
            doc_intro("Capable of processing time-series messages and responding accordingly")
            doc_intro("The user can setup callback to python to handle unbound symbolic time-series references")
            doc_intro("- that typically involve reading time-series from a service or storage for the specified period")
            doc_intro("The server object will then compute the resulting time-series vector,")
            doc_intro("and respond back to clients with the results")
            doc_see_also("DtsServer"),no_init
            )
            .def(init<std::string>(args("host_port"),
                doc_intro("constructs a dts-client with the specifed host_port parameter")
                doc_parameter("host_port","string", "a string of the format 'host:portnumber', e.g. 'localhost:20000'")
                )
             )
            .def("close",&DtsClient::close,(boost::python::arg("timeout_ms")=1000),
                doc_intro("close the connection")
            )
            .def("percentiles",&DtsClient::percentiles, args("ts_vector","utcperiod","time_axis","percentile_list"),
                doc_intro("Evaluates the expressions in the ts_vector for the specified utcperiod.")
                doc_intro("If the expression includes unbound symbolic references to time-series,")
                doc_intro("these time-series will be passed to the binding service callback")
                doc_intro("on the serverside.")
                doc_parameters()
                doc_parameter("ts_vector","TsVector","a list of time-series (expressions), including unresolved symbolic references")
                doc_parameter("utcperiod","UtcPeriod","the period that the binding service should read from the backing ts-store/ts-service")
                doc_parameter("time_axis","TimeAxis","the time_axis for the percentiles, e.g. a weekly time_axis")
                doc_parameter("percentile_list","IntVector","a list of percentiles, where -1 means true average, 25=25percentile etc")
                doc_returns("tsvector","TsVector","an evaluated list of percentile time-series in the same order as the percentile input list")
                doc_see_also(".evaluate(), DtsServer")
            )
            .def("evaluate", &DtsClient::evaluate, args("ts_vector","utcperiod"),
                doc_intro("Evaluates the expressions in the ts_vector for the specified utcperiod.")
                doc_intro("If the expression includes unbound symbolic references to time-series,")
                doc_intro("these time-series will be passed to the binding service callback")
                doc_intro("on the serverside.")
                doc_parameters()
                doc_parameter("ts_vector","TsVector","a list of time-series (expressions), including unresolved symbolic references")
                doc_parameter("utcperiod","UtcPeriod","the period that the binding service should read from the backing ts-store/ts-service")
                doc_returns("tsvector","TsVector","an evaluated list of point time-series in the same order as the input list")
                doc_see_also(".percentiles(),DtsServer")
            )
            ;

    }

    void dtss() {
        dtss_messages();
        dtss_server();
        dtss_client();
    }
}
