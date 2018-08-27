#include "mgos_gcp_remote_config.h"
#include "common/cs_dbg.h"
#include "mgos_event.h"
#include "mgos_mqtt.h"
#include "mgos_remote_config.h"
#include "mgos_sys_config.h"
#include "mgos_system.h"
#include "mgos_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gcp_config_state {
  struct mgos_rlock_type *lock;
  struct mg_str config_topic;
  unsigned int sub_id : 16;
  bool connected : 1;
};

static void mgos_gcp_rcfg_mqtt_ev(struct mg_connection *nc, int ev,
                                  void *ev_data, void *user_data) {
  struct gcp_config_state *ss = (struct gcp_config_state *)user_data;
  mgos_rlock(ss->lock);

  switch (ev) {
    case MG_EV_MQTT_CONNACK: {
      struct mg_mqtt_topic_expression topic = {.topic = ss->config_topic.p,
                                               .qos = 1};
      ss->sub_id = mgos_mqtt_get_packet_id();
      mg_mqtt_subscribe(nc, &topic, 1, ss->sub_id);
      break;
    }

    case MG_EV_MQTT_SUBACK: {
      struct mg_mqtt_message *msg = (struct mg_mqtt_message *)ev_data;
      if (msg->message_id != ss->sub_id || ss->connected)
        break;

      ss->connected = true;
      break;
    }

    case MG_EV_MQTT_PUBLISH: {
      struct mg_mqtt_message *msg = (struct mg_mqtt_message *)ev_data;
      if (mg_strcmp(msg->topic, ss->config_topic) == 0) {
        mg_mqtt_puback(nc, msg->message_id);
        struct mg_str config = msg->payload;

        LOG(LL_DEBUG, ("New config: '%.*s'", (int)config.len, config.p));
        if (config.len > 0) {
          mgos_event_trigger(MGOS_REMOTE_CONFIG_UPDATE_FULL, &config);
        } else {
          LOG(LL_DEBUG, ("config was empty, ignoring"));
        }
      }

      break;
    }

    default:
      break;
  }

  mgos_runlock(ss->lock);
}

bool mgos_gcp_remote_config_init(void) {
  if (!mgos_sys_config_get_rcfg_enable()) {
    return true;
  }

  if (!mgos_sys_config_get_gcp_enable()) {
    LOG(LL_DEBUG, ("GCP remote config requires gcp.enable"));
    return true;
  }

  const char *impl = mgos_sys_config_get_rcfg_lib();
  if (impl != NULL && strcmp(impl, "gcp") != 0) {
    LOG(LL_DEBUG, ("rcfg.lib=%s, not initialising GCP remote config", impl));
    return true;
  }

  const char *device = mgos_sys_config_get_gcp_device();
  if (device == NULL) {
    LOG(LL_ERROR, ("gcp.device is NULL, not enabling remote config"));
    return false;
  }

  char *topic_str = NULL;
  int len = mg_asprintf(&topic_str, 0, "/devices/%s/config", device);
  struct mg_str topic = mg_mk_str_n(topic_str, len);

  struct gcp_config_state *ss =
      (struct gcp_config_state *)calloc(1, sizeof(*ss));
  ss->config_topic = topic;
  ss->lock = mgos_rlock_create();
  mgos_mqtt_add_global_handler(mgos_gcp_rcfg_mqtt_ev, ss);

  LOG(LL_INFO,
      ("GCP Device Config enabled, topic: %.*s", (int)topic.len, topic.p));

  return true;
}
