import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import media_player
from esphome.const import CONF_ID
from . import gensam_ns, GenSAMHub

AUTO_LOAD = ["gensam"]

CONF_GENSAM_ID = "gensam_id"

GenSAMMediaPlayer = gensam_ns.class_("GenSAMMediaPlayer", media_player.MediaPlayer, cg.Component)

CONFIG_SCHEMA = media_player.media_player_schema(GenSAMMediaPlayer).extend(
    {
        cv.GenerateID(CONF_GENSAM_ID): cv.use_id(GenSAMHub),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)

    hub = await cg.get_variable(config[CONF_GENSAM_ID])
    cg.add(var.set_hub(hub))
