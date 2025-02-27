/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "redpanda/admin_server.h"

#include "cluster/cluster_utils.h"
#include "cluster/controller.h"
#include "cluster/controller_api.h"
#include "cluster/fwd.h"
#include "cluster/metadata_cache.h"
#include "cluster/partition_manager.h"
#include "cluster/security_frontend.h"
#include "cluster/shard_table.h"
#include "cluster/topics_frontend.h"
#include "cluster/types.h"
#include "config/configuration.h"
#include "config/endpoint_tls_config.h"
#include "model/namespace.h"
#include "raft/types.h"
#include "redpanda/admin/api-doc/broker.json.h"
#include "redpanda/admin/api-doc/config.json.h"
#include "redpanda/admin/api-doc/kafka.json.h"
#include "redpanda/admin/api-doc/partition.json.h"
#include "redpanda/admin/api-doc/raft.json.h"
#include "redpanda/admin/api-doc/security.json.h"
#include "redpanda/admin/api-doc/status.json.h"
#include "rpc/dns.h"
#include "security/scram_algorithm.h"
#include "security/scram_authenticator.h"
#include "vlog.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/http/api_docs.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/json_path.hh>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <rapidjson/document.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <unordered_map>

using namespace std::chrono_literals;

static ss::logger logger{"admin_api_server"};

admin_server::admin_server(
  admin_server_cfg cfg,
  ss::sharded<cluster::partition_manager>& pm,
  cluster::controller* controller,
  ss::sharded<cluster::shard_table>& st,
  ss::sharded<cluster::metadata_cache>& metadata_cache)
  : _server("admin")
  , _cfg(std::move(cfg))
  , _partition_manager(pm)
  , _controller(controller)
  , _shard_table(st)
  , _metadata_cache(metadata_cache) {}

ss::future<> admin_server::start() {
    configure_metrics_route();
    configure_dashboard();
    configure_admin_routes();

    co_await configure_listeners();

    vlog(
      logger.info,
      "Started HTTP admin service listening at {}",
      _cfg.endpoints);
}

ss::future<> admin_server::stop() { return _server.stop(); }

void admin_server::configure_admin_routes() {
    auto rb = ss::make_shared<ss::api_registry_builder20>(
      _cfg.admin_api_docs_dir, "/v1");

    auto insert_comma = [](ss::output_stream<char>& os) {
        return os.write(",\n");
    };
    rb->set_api_doc(_server._routes);
    rb->register_api_file(_server._routes, "header");
    rb->register_api_file(_server._routes, "config");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "raft");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "kafka");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "partition");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "security");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "status");
    ss::httpd::config_json::get_config.set(
      _server._routes, []([[maybe_unused]] ss::const_req req) {
          rapidjson::StringBuffer buf;
          rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
          config::shard_local_cfg().to_json(writer);
          return ss::json::json_return_type(buf.GetString());
      });
    register_raft_routes();
    register_kafka_routes();
    register_security_routes();
    register_status_routes();
    register_broker_routes();
    register_partition_routes();
}

void admin_server::configure_dashboard() {
    if (_cfg.dashboard_dir) {
        _dashboard_handler = std::make_unique<dashboard_handler>(
          *_cfg.dashboard_dir);
        _server._routes.add(
          ss::httpd::operation_type::GET,
          ss::httpd::url("/dashboard").remainder("path"),
          _dashboard_handler.get());
    }
}

void admin_server::configure_metrics_route() {
    ss::prometheus::config metrics_conf;
    metrics_conf.metric_help = "redpanda metrics";
    metrics_conf.prefix = "vectorized";
    ss::prometheus::add_prometheus_routes(_server, metrics_conf).get();
}

ss::future<> admin_server::configure_listeners() {
    for (auto& ep : _cfg.endpoints) {
        // look for credentials matching current endpoint
        auto tls_it = std::find_if(
          _cfg.endpoints_tls.begin(),
          _cfg.endpoints_tls.end(),
          [&ep](const config::endpoint_tls_config& c) {
              return c.name == ep.name;
          });

        ss::shared_ptr<ss::tls::server_credentials> cred;
        if (tls_it != _cfg.endpoints_tls.end()) {
            auto builder = co_await tls_it->config.get_credentials_builder();
            if (builder) {
                cred = co_await builder->build_reloadable_server_credentials(
                  [](
                    const std::unordered_set<ss::sstring>& updated,
                    const std::exception_ptr& eptr) {
                      cluster::log_certificate_reload_event(
                        logger, "API TLS", updated, eptr);
                  });
            }
        }
        auto resolved = co_await rpc::resolve_dns(ep.address);
        co_await ss::with_scheduling_group(_cfg.sg, [this, cred, resolved] {
            return _server.listen(resolved, cred);
        });
    }
}

void admin_server::register_raft_routes() {
    ss::httpd::raft_json::raft_transfer_leadership.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          raft::group_id group_id;
          try {
              group_id = raft::group_id(std::stoll(req->param["group_id"]));
          } catch (...) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Raft group id must be an integer: {}",
                req->param["group_id"]));
          }

          if (group_id() < 0) {
              throw ss::httpd::bad_param_exception(
                fmt::format("Invalid raft group id {}", group_id));
          }

          if (!_shard_table.local().contains(group_id)) {
              throw ss::httpd::not_found_exception(
                fmt::format("Raft group {} not found", group_id));
          }

          std::optional<model::node_id> target;
          if (auto node = req->get_query_param("target"); !node.empty()) {
              try {
                  target = model::node_id(std::stoi(node));
              } catch (...) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Target node id must be an integer: {}", node));
              }
              if (*target < 0) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Invalid target node id {}", *target));
              }
          }

          vlog(
            logger.info,
            "Leadership transfer request for raft group {} to node {}",
            group_id,
            target);

          auto shard = _shard_table.local().shard_for(group_id);

          return _partition_manager.invoke_on(
            shard, [group_id, target](cluster::partition_manager& pm) mutable {
                auto consensus = pm.consensus_for(group_id);
                if (!consensus) {
                    throw ss::httpd::not_found_exception();
                }
                return consensus->transfer_leadership(target).then(
                  [](std::error_code err) {
                      if (err) {
                          throw ss::httpd::server_error_exception(fmt::format(
                            "Leadership transfer failed: {}", err.message()));
                      }
                      return ss::json::json_return_type(ss::json::json_void());
                  });
            });
      });
}

// TODO: factor out generic serialization from seastar http exceptions
static security::scram_credential
parse_scram_credential(const rapidjson::Document& doc) {
    if (!doc.IsObject()) {
        throw ss::httpd::bad_request_exception(fmt::format("Not an object"));
    }

    if (!doc.HasMember("algorithm") || !doc["algorithm"].IsString()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("String algo missing"));
    }
    const auto algorithm = std::string_view(
      doc["algorithm"].GetString(), doc["algorithm"].GetStringLength());

    if (!doc.HasMember("password") || !doc["password"].IsString()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("String password smissing"));
    }
    const auto password = doc["password"].GetString();

    security::scram_credential credential;

    if (algorithm == security::scram_sha256_authenticator::name) {
        credential = security::scram_sha256::make_credentials(
          password, security::scram_sha256::min_iterations);

    } else if (algorithm == security::scram_sha512_authenticator::name) {
        credential = security::scram_sha512::make_credentials(
          password, security::scram_sha512::min_iterations);

    } else {
        throw ss::httpd::bad_request_exception(
          fmt::format("Unknown scram algorithm: {}", algorithm));
    }

    return credential;
}

void admin_server::register_security_routes() {
    ss::httpd::security_json::create_user.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          rapidjson::Document doc;
          doc.Parse(req->content.data());

          auto credential = parse_scram_credential(doc);

          if (!doc.HasMember("username") || !doc["username"].IsString()) {
              throw ss::httpd::bad_request_exception(
                fmt::format("String username missing"));
          }

          auto username = security::credential_user(
            doc["username"].GetString());

          return _controller->get_security_frontend()
            .local()
            .create_user(username, credential, model::timeout_clock::now() + 5s)
            .then([](std::error_code err) {
                vlog(logger.debug, "Creating user {}:{}", err, err.message());
                if (err) {
                    throw ss::httpd::bad_request_exception(
                      fmt::format("Creating user: {}", err.message()));
                }
                return ss::make_ready_future<ss::json::json_return_type>(
                  ss::json::json_return_type(ss::json::json_void()));
            });
      });

    ss::httpd::security_json::delete_user.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto user = security::credential_user(
            model::topic(req->param["user"]));

          return _controller->get_security_frontend()
            .local()
            .delete_user(user, model::timeout_clock::now() + 5s)
            .then([](std::error_code err) {
                vlog(logger.debug, "Deleting user {}:{}", err, err.message());
                if (err) {
                    throw ss::httpd::bad_request_exception(
                      fmt::format("Deleting user: {}", err.message()));
                }
                return ss::make_ready_future<ss::json::json_return_type>(
                  ss::json::json_return_type(ss::json::json_void()));
            });
      });

    ss::httpd::security_json::update_user.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto user = security::credential_user(
            model::topic(req->param["user"]));

          rapidjson::Document doc;
          doc.Parse(req->content.data());

          auto credential = parse_scram_credential(doc);

          return _controller->get_security_frontend()
            .local()
            .update_user(user, credential, model::timeout_clock::now() + 5s)
            .then([](std::error_code err) {
                vlog(logger.debug, "Updating user {}:{}", err, err.message());
                if (err) {
                    throw ss::httpd::bad_request_exception(
                      fmt::format("Updating user: {}", err.message()));
                }
                return ss::make_ready_future<ss::json::json_return_type>(
                  ss::json::json_return_type(ss::json::json_void()));
            });
      });

    ss::httpd::security_json::list_users.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request>) {
          std::vector<ss::sstring> users;
          for (const auto& [user, _] :
               _controller->get_credential_store().local()) {
              users.push_back(user());
          }
          return ss::make_ready_future<ss::json::json_return_type>(
            std::move(users));
      });
}

void admin_server::register_kafka_routes() {
    ss::httpd::kafka_json::kafka_transfer_leadership.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto topic = model::topic(req->param["topic"]);

          model::partition_id partition;
          try {
              partition = model::partition_id(
                std::stoll(req->param["partition"]));
          } catch (...) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Partition id must be an integer: {}",
                req->param["partition"]));
          }

          if (partition() < 0) {
              throw ss::httpd::bad_param_exception(
                fmt::format("Invalid partition id {}", partition));
          }

          std::optional<model::node_id> target;
          if (auto node = req->get_query_param("target"); !node.empty()) {
              try {
                  target = model::node_id(std::stoi(node));
              } catch (...) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Target node id must be an integer: {}", node));
              }
              if (*target < 0) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Invalid target node id {}", *target));
              }
          }

          vlog(
            logger.info,
            "Leadership transfer request for leader of topic-partition {}:{} "
            "to node {}",
            topic,
            partition,
            target);

          model::ntp ntp(model::kafka_namespace, topic, partition);

          auto shard = _shard_table.local().shard_for(ntp);
          if (!shard) {
              throw ss::httpd::not_found_exception(fmt::format(
                "Topic partition {}:{} not found", topic, partition));
          }

          return _partition_manager.invoke_on(
            *shard,
            [ntp = std::move(ntp),
             target](cluster::partition_manager& pm) mutable {
                auto partition = pm.get(ntp);
                if (!partition) {
                    throw ss::httpd::not_found_exception();
                }
                return partition->transfer_leadership(target).then(
                  [](std::error_code err) {
                      if (err) {
                          throw ss::httpd::server_error_exception(fmt::format(
                            "Leadership transfer failed: {}", err.message()));
                      }
                      return ss::json::json_return_type(ss::json::json_void());
                  });
            });
      });
}

void admin_server::register_status_routes() {
    ss::httpd::status_json::ready.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request>) {
          std::unordered_map<ss::sstring, ss::sstring> status_map{
            {"status", _ready ? "ready" : "booting"}};
          return ss::make_ready_future<ss::json::json_return_type>(status_map);
      });
}

void admin_server::register_broker_routes() {
    ss::httpd::broker_json::get_brokers.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request>) {
          std::vector<ss::httpd::broker_json::broker> res;
          for (const auto& broker : _metadata_cache.local().all_brokers()) {
              auto& b = res.emplace_back();
              b.node_id = broker->id();
              b.num_cores = broker->properties().cores;
          }
          return ss::make_ready_future<ss::json::json_return_type>(
            std::move(res));
      });
}

struct json_validator {
    explicit json_validator(const std::string& schema_text)
      : schema(make_schema_document(schema_text))
      , validator(schema) {}

    static rapidjson::SchemaDocument
    make_schema_document(const std::string& schema) {
        rapidjson::Document doc;
        if (doc.Parse(schema).HasParseError()) {
            throw std::runtime_error(
              fmt::format("Invalid schema document: {}", schema));
        }
        return rapidjson::SchemaDocument(doc);
    }

    const rapidjson::SchemaDocument schema;
    rapidjson::SchemaValidator validator;
};

static json_validator make_set_replicas_validator() {
    const std::string schema = R"(
{
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "node_id": {
                "type": "number"
            },
            "core": {
                "type": "number"
            }
        },
        "required": [
            "node_id",
            "core"
        ],
        "additionalProperties": false
    }
}
)";
    return json_validator(schema);
}

void admin_server::register_partition_routes() {
    /*
     * Get a list of partition summaries.
     */
    ss::httpd::partition_json::get_partitions.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request>) {
          using summary = ss::httpd::partition_json::partition_summary;
          return _partition_manager
            .map_reduce0(
              [](cluster::partition_manager& pm) {
                  std::vector<summary> partitions;
                  partitions.reserve(pm.partitions().size());
                  for (const auto& it : pm.partitions()) {
                      summary p;
                      p.ns = it.first.ns;
                      p.topic = it.first.tp.topic;
                      p.partition_id = it.first.tp.partition;
                      p.core = ss::this_shard_id();
                      partitions.push_back(std::move(p));
                  }
                  return partitions;
              },
              std::vector<summary>{},
              [](std::vector<summary> acc, std::vector<summary> update) {
                  acc.insert(acc.end(), update.begin(), update.end());
                  return acc;
              })
            .then([](std::vector<summary> partitions) {
                return ss::make_ready_future<ss::json::json_return_type>(
                  std::move(partitions));
            });
      });

    /*
     * Get detailed information about a partition.
     */
    ss::httpd::partition_json::get_partition.set(
      _server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto ns = model::ns(req->param["namespace"]);
          auto topic = model::topic(req->param["topic"]);

          model::partition_id partition;
          try {
              partition = model::partition_id(
                std::stoi(req->param["partition"]));
          } catch (...) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Partition id must be an integer: {}",
                req->param["partition"]));
          }

          if (partition() < 0) {
              throw ss::httpd::bad_param_exception(
                fmt::format("Invalid partition id {}", partition));
          }

          const model::ntp ntp(std::move(ns), std::move(topic), partition);

          if (!_metadata_cache.local().contains(ntp)) {
              throw ss::httpd::not_found_exception(
                fmt::format("Could not find ntp: {}", ntp));
          }

          ss::httpd::partition_json::partition p;
          p.ns = ntp.ns;
          p.topic = ntp.tp.topic;
          p.partition_id = ntp.tp.partition;

          auto assignment
            = _controller->get_topics_state().local().get_partition_assignment(
              ntp);

          if (assignment) {
              for (auto& r : assignment->replicas) {
                  ss::httpd::partition_json::assignment a;
                  a.node_id = r.node_id;
                  a.core = r.shard;
                  p.replicas.push(a);
              }
          }

          return _controller->get_api()
            .local()
            .get_reconciliation_state(ntp)
            .then([p](const cluster::ntp_reconciliation_state& state) mutable {
                p.status = ssx::sformat("{}", state.status());
                return ss::make_ready_future<ss::json::json_return_type>(
                  std::move(p));
            });
      });

    // make sure to call reset() before each use
    static thread_local json_validator set_replicas_validator(
      make_set_replicas_validator());

    ss::httpd::partition_json::set_partition_replicas.set(
      _server._routes,
      [this](std::unique_ptr<ss::httpd::request> req)
        -> ss::future<ss::json::json_return_type> {
          auto ns = model::ns(req->param["namespace"]);
          auto topic = model::topic(req->param["topic"]);

          model::partition_id partition;
          try {
              partition = model::partition_id(
                std::stoi(req->param["partition"]));
          } catch (...) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Partition id must be an integer: {}",
                req->param["partition"]));
          }

          if (partition() < 0) {
              throw ss::httpd::bad_param_exception(
                fmt::format("Invalid partition id {}", partition));
          }

          if (ns != model::kafka_namespace) {
              throw ss::httpd::bad_request_exception(
                fmt::format("Unsupported namespace: {}", ns));
          }

          rapidjson::Document doc;
          if (doc.Parse(req->content.data()).HasParseError()) {
              throw ss::httpd::bad_request_exception(
                "Could not replica set json");
          }

          set_replicas_validator.validator.Reset();
          if (!doc.Accept(set_replicas_validator.validator)) {
              throw ss::httpd::bad_request_exception(
                "Replica set json is invalid");
          }

          std::vector<model::broker_shard> replicas;
          for (auto& r : doc.GetArray()) {
              replicas.push_back(model::broker_shard{
                .node_id = model::node_id(r["node_id"].GetInt()),
                .shard = static_cast<uint32_t>(r["core"].GetInt()),
              });
          }

          const model::ntp ntp(std::move(ns), std::move(topic), partition);

          vlog(
            logger.info,
            "Request to change ntp {} replica set to {}",
            ntp,
            replicas);

          auto err
            = co_await _controller->get_topics_frontend()
                .local()
                .move_partition_replicas(
                  ntp,
                  replicas,
                  model::timeout_clock::now()
                    + 10s); // NOLINT(cppcoreguidelines-avoid-magic-numbers)

          if (err) {
              vlog(
                logger.error,
                "Error changing ntp {} replicas: {}:{}",
                ntp,
                err,
                err.message());
              throw ss::httpd::bad_request_exception(
                fmt::format("Error moving partition: {}", err.message()));
          }

          co_return ss::json::json_void();
      });
}
