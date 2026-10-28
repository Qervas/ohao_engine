---
module: audio
id: schedule
title: The voice schedule: sound decided in the step
standard: v2
figures: []
---

## What plays is decided in the step

A sound in OHAO is not played where its cause happens. The step's physics,
combat and animation write their events; the last simulate system,
`audio_schedule`, reads them and decides every sound: which bank sound, which
variant, where, how loud, behind how many walls, and which voice it takes. It
writes commands into a log; nothing in the step touches a device.

{{cite ohao/ecs/audio_schedule.cpp "[system](World&, float) { system->step(); });"}}

Footsteps and landings come from the physics log, rounds, hits and deaths from
the combat log after the step's last `restart`, and `mag_out`, `mag_in`, `bolt`,
`body_fall` and a character's `foot_l`/`foot_r` from the animation log. The
mapping from an event to a sound name is a few lines of code; everything about
the sound itself is the bank's:

{{cite ohao/ecs/audio_schedule.cpp "std::vector<std::string> animSoundNames(const AnimEvent& e) {"}}

:::why
Deciding in the step makes a sound a function of the simulation. A window, a
headless host and a replay of the same input step the same world, so they write
the same commands: `check_live` compares a window's schedule with a headless
session's command for command. A sound can be tested without a device, by
reading the log.
:::

## Priority, caps and how long a voice is held

A bank sound names its bus, a priority and `holdMs`, the time it holds a voice.
Buses have caps and the bank a global one. A new voice that would pass a cap
looks for a victim in that cap's scope: the lowest priority goes, then the
quietest at the listener, then the oldest. If nothing in the scope is lower, the
new voice is dropped, and the command says so:

{{cite ohao/ecs/audio_schedule.cpp "if (v.priority < p.priority || (v.priority == p.priority && (v.audible < p.audible || (v.audible == p.audible && v.seq < p.seq))))"}}

A stolen voice gets a `stop` before the new `play`, so the device never holds
more than the caps allow. Ducks are commands too: each bus takes the deepest
duck rule holding it, attacking down and releasing up. The round's mix is a
snapshot (`default`, `dead`, `won`) the schedule writes when the round changes.

## Occlusion has a budget

A sound behind walls is cut by how many: none; −8 dB with a 1.2 kHz low-pass;
−14 dB with 700 Hz for two or more. Counting walls takes a ray, and a step casts
at most 16. Past them a new voice counts as unoccluded and a held one keeps its
last answer; both are counted (`rays.capped`). A held voice is re-cast every 6
steps:

{{cite ohao/ecs/audio_schedule.hpp "constexpr uint32_t kRaysPerStep = 16;         // occlusion rays a step"}}
{{cite ohao/ecs/audio_schedule.cpp "if (raysThisStep >= kRaysPerStep) {"}}

The listener's reverb zone is the same kind of decision: when the listener
crosses into another `ReverbZone`, a `zone` command names its preset and bed, with
a 30-step blend.

## A restore is heard

The schedule's state is runtime, never hashed, but it rides the World's
snapshot. `installAudio` chains its own state in front of the hook that was
there (physics'), so one snapshot carries both:

{{cite ohao/ecs/audio_schedule.cpp "if (previous.restore) previous.restore(both ? both->previous : nullptr);"}}

After a round's restart or any restore, the step's first command is
`stop_world`, a short fade of the world's voices; after a restore the schedule
also resends the mix it now holds as `resync` commands, which a player already
there ignores:

{{cite ohao/ecs/audio_schedule.cpp "c.op = AudioCommand::Op::StopWorld;"}}

A restore mid-fight and the same steps after it write what a recording wrote
(`t28_*`). `world.hash` never changes with the schedule present or absent.

## The device renders it a step later

A window's `AudioPlayer` reads the commands after its cursor once a frame and
plays each one step after its step, on a clock anchored to the device's frames.
A command whose time has already passed plays at once and is counted late:

{{cite ohao/runtime/audio_player.cpp "if (at < static_cast<int64_t>(now)) {   // its time has passed: at once, counted"}}

The clock is anchored again when the timeline is not running in real time, when
the step goes back (a restore), and when it drifts more than a step from the
device. The player opens no file on a play: the bank's sounds are decoded when
the bank loads.

## Agents read the log and override the device

`audio.info` is the schedule's half, in any host: the snapshot, zone, listener,
buses with their duck, voice and ray counts, and the commands after a cursor.
Headless it says `device: "none"`; a window adds the device's sample rate,
late commands, the mix's cost and each bus's driven level. `audio.mix` sets
listening overrides (a bus's level, mute or solo, a forced snapshot). The
session keeps them and hands them whole to the host, whose player applies them
in the device only:

{{cite ohao/api/audio.cpp "if (s.render) s.render->setAudioMix(s.audioMix);"}}

They never reach the World, the schedule or a snapshot, so a test can solo a
bus mid-fight and the commands, `world.hash` and replays stay as they were
(`t29_audio_mix`).

## Contracts

- Nothing on the bank's path may set pitch: voices and groups are made with `MA_SOUND_FLAG_NO_PITCH`, because the pitch resampler delays a voice by a frame per node and the player's start frames would land late.
- A sound plays from the step's events, never from a host's own reading of the world: a window that played a footstep itself would differ from a headless session's schedule.
- The schedule is the step's last simulate system; a system added after it writes events it never sees.
- The schedule's state is restored only through the World's snapshot; a host that restores physics by hand leaves the schedule's voices and cursor behind.
