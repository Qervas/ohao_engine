# The footsteps module (footsteps/module.hpp): the player's steps and landings and the characters' clip feet, sounded
# through the audio schedule.
ohao_add_module(footsteps DEPENDS ecs animation physics audio LINKS ohao_audio_bank)
