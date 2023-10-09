#ifndef _ROUTER_ROUTER_HPP
#define _ROUTER_ROUTER_HPP

#include <atomic>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <baseTypes.hpp>
#include <parseEvent.hpp>
#include <queue/concurrentQueue.hpp>
#include <store/istore.hpp>

#include "policyManager.hpp"
#include "route.hpp"

namespace router
{
constexpr auto ROUTES_TABLE_NAME = "internal/router_table/0"; ///< Name of the routes table in the store
constexpr auto JSON_PATH_NAME = "/name";                      ///< Json path for the name of the route
constexpr auto JSON_PATH_FILTER = "/filter";                  ///< Json path for the filter of the route
constexpr auto JSON_PATH_PRIORITY = "/priority";              ///< Json path for the priority of the route
constexpr auto JSON_PATH_TARGET = "/target";                  ///< Json path for the target of the route

constexpr auto JSON_PATH_EVENT = "/event";                    ///< Json path for the event for enqueue

/**
 * @brief Router class to manage routes and events
 *
 * The router is the main class of the router module. It manages the routes and has the logic to route the events to the
 * correct policy. Also it has the thread pool to process the events, and the policy manager to manage the
 * runtime policies (creation, destruction, and interaction).
 * Get the events from the queue, select the correct policy (with the route priority and conditions), and send the
 * event to the policy.
 */
class Router
{

private:
    using concurrentQueue = base::queue::ConcurrentQueue<base::Event>; ///< Alias for the queue type

    /* Status */
    /**
     * @brief Map of routes, each route is a tuple with the priority and the filter name
     */
    std::unordered_map<std::string, std::tuple<std::size_t, std::string>> m_namePriorityFilter;
    /**
     * @brief Map of routes, each route (priority) is a vector of expressions, each expression for each thread
     * The map is sorted by priority
     */
    std::map<std::size_t, std::vector<Route>> m_priorityRoute;
    std::shared_mutex m_mutexRoutes;    ///< Mutex to protect the routes map
    std::atomic_bool m_isRunning;       ///< Flag to know if the router is running
    std::vector<std::thread> m_threads; ///< Vector of threads for the router

    /* Resources */
    std::shared_ptr<PolicyManager> m_policyManager; ///< Policy manager
    std::shared_ptr<builder::Builder> m_builder;    ///< Builder
    std::shared_ptr<concurrentQueue> m_queue;       ///< Queue to get events
    std::shared_ptr<store::IStore> m_store;         ///< Store to get/save routes table

    /* Config */
    std::size_t m_numThreads; ///< Number of threads for the router

    /**
     * @brief Get a Json with the routes table
     *
     * Array of objects with the name, priority and target of each route
     * The array is sorted by priority
     * @return json::Json with the routes table
     */
    json::Json tableToJson();

    /**
     * @brief Dump the routes table to the store
     * @warning This method is not thread safe. This method exits the program if the store fails
     */
    void dumpTableToStorage();

public:
    using Entry =
        std::tuple<std::string, std::size_t, std::string, std::string, std::string>; ///< Entry of the routes table
                                                                                     ///< [name, priority, filter,
                                                                                     ///< target, hash]

    /**
     * @brief Construct a new Router with the given builder, store and number of threads for the pool
     *
     * @param builder Builder to create the policies
     * @param store Store to get/save the routes table
     * @param threads Number of threads for the pool
     */
    Router(std::shared_ptr<builder::Builder> builder, std::shared_ptr<store::IStore> store, std::size_t threads = 1);

    /**
     * @brief Get the list of route names, priority and target
     *
     * @return std::unordered_set<std::string>
     */
    std::vector<Entry> getRouteTable();

    /**
     * @brief Get the Entry of a route by name
     *
     * @param name name of the route
     * @return std::optional<entry> A tuple with the name, priority and target of the route or nullopt if the route
     * does not exist
     */
    std::optional<Entry> getEntry(const std::string& name);

    /**
     * @brief Change the priority of a route
     *
     * @param name name of the route
     * @param priority new priority
     * @return std::optional<base::Error> A error with description if the route can't be changed
     */
    std::optional<base::Error> changeRoutePriority(const std::string& name, int priority);

    /**
     * @brief Add a new route to the router.
     *
     * Optionally, the priority can be specified. If not, the priority is the especified in the route.
     * If the route already exists or the priority is already used, the route is not
     * added.
     * @param name name of the route
     * @return A error with description if the route can't be added
     * // TODO: FIX ALL DOC --
     */
    std::optional<base::Error>
    addRoute(const std::string& routeName, int priority, const std::string& filterName, const std::string& envName);

    /**
     * @brief Push an event to the queue of the router
     *
     * @param event event to push to the queue
     * @return std::optional<base::Error> A error with description if the event can't be pushed
     */
    std::optional<base::Error> enqueueEvent(base::Event&& event);

    /**
     * @brief Push an event to the queue of the router
     *
     * @param event event to push to the queue in Wazuh format: <queue>:<location>:<data>
     * @return std::optional<base::Error> A error with description if the event can't be pushed
     */
    std::optional<base::Error> enqueueWazuhEvent(std::string_view event);

    /**
     * @brief Push an event to the queue of the router
     *
     * This method is inline and does not check the router && queue status
     * @param event
     */
    void fastEnqueueEvent(const std::string& eventStr)
    {
        base::Event event;
        try
        {
            event = base::parseEvent::parseWazuhEvent(eventStr);
        }
        catch (const std::exception& e)
        {
            LOG_WARNING("Error parsing event: '{}' (discarding...)", e.what());
            return;
        }
        m_queue->push(std::move(event));
    }

    /**
     * @brief Delete a route from the router
     *
     * @param name name of the route
     * @return A error with description if the route can't be deleted
     */
    void removeRoute(const std::string& name);

    /**
     * @brief Launch in a new threads the router to ingest data from the queue.
     */
    std::optional<base::Error> run(std::shared_ptr<concurrentQueue> queue);

    /**
     * @brief Stop the router
     *
     * Send a stop signal to the router and wait for the threads to finish.
     */
    void stop();

    /**
     * @brief Clear the router table
     *
     */
    void clear();

    /**
     * @brief Subscribes to a callback that generates the outputs and traces corresponding to each event.
     *
     * This function subscribes to a callback mechanism that generates the outputs and traces corresponding
     * to each event for a specified policy and its instance. The generated output and trace data will be
     * passed to the provided `outputCallback` and `traceCallback` functions.
     *
     * @param outputCallback The callback function to be invoked with the generated output data.
     * @param traceCallback The callback function to be invoked with the generated trace data.
     * @param assets Vector of asset names to subscribe to.
     * @param policyName Name of the policy for which output and trace are to be obtained.
     * @return std::optional<base::Error> If the policy is not configured for test mode, an error is returned.
     *         Otherwise, returns std::nullopt if the subscription was successful.
     */
    std::optional<base::Error> subscribeOutputAndTraces(const OutputSubscriber& outputCallback,
                                                        const bk::Subscriber& traceCallback,
                                                        const std::vector<std::string>& assets,
                                                        const std::string& policyName);

    /**
     * @brief Get the Assets object
     *
     * @param policyName
     * @return base::RespOrError<std::vector<std::string>>
     */
    base::RespOrError<std::vector<std::string>> getAssets(const std::string& policyName) const;

    /**
     * @brief
     *
     * @param policyName
     * @return base::OptError
     */
    base::OptError unsubscribe(const std::string& policyName);

    /**
     * @brief Get a hash of the policy
     *
     * @param policyName Name of the policy to get the hash
     * @return std::optional<std::string> Hash of the policy or nullopt if the policy does not exist or is invalid name
     */
    std::optional<std::string> getPolicyHash(const std::string& policyName) const;
};
} // namespace router
#endif // _ROUTER_ROUTER_HPP
