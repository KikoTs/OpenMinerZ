using System;
using System.Collections.Concurrent;
using System.Globalization;
using System.IO;
using System.Threading;
using Microsoft.Xna.Framework.Audio;

namespace OpenMinerZ.AudioBridge
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            if (args.Length < 1 || args.Length > 2)
            {
                Console.Error.WriteLine(
                    "usage: OpenMinerZ.AudioRuntime <content pack directory> [renderer id]");
                return 2;
            }

            string content = Path.Combine(Path.GetFullPath(args[0]), "Content");
            bool trace = String.Equals(Environment.GetEnvironmentVariable(
                "OPENMINERZ_AUDIO_TRACE"), "1", StringComparison.Ordinal);
            try
            {
                AudioEngine audioEngine = args.Length == 2 && args[1].Length > 0
                    ? new AudioEngine(Path.Combine(content, "Sounds.xgs"),
                        TimeSpan.FromMilliseconds(250.0), args[1])
                    : new AudioEngine(Path.Combine(content, "Sounds.xgs"));
                using (var engine = audioEngine)
                using (var waveBank = new WaveBank(engine, Path.Combine(content, "Sounds.xwb")))
                using (var streamingWaveBank = new WaveBank(engine,
                    Path.Combine(content, "SoundsStreaming.xwb"), 0, 32))
                using (var soundBank = new SoundBank(engine, Path.Combine(content, "Sounds.xsb")))
                {
                    do
                    {
                        engine.Update();
                    }
                    while (!waveBank.IsPrepared || !streamingWaveBank.IsPrepared);

                    AudioCategory effects = engine.GetCategory("Default");
                    AudioCategory music = engine.GetCategory("Music");
                    AudioCategory[] ambience =
                    {
                        engine.GetCategory("AmbientDay"), engine.GetCategory("AmbientNight"),
                        engine.GetCategory("AmbientCave"), engine.GetCategory("AmbientHell")
                    };
                    // A live process is not enough: signal the native client only after
                    // XACT opened both wave banks and resolved every required category.
                    // Include the renderer XACT actually opened so the native client
                    // diagnoses mute/volume on that device rather than a different role.
                    string activeRendererId = args.Length == 2 ? args[1] : "";
                    string activeRendererName = "XACT default";
                    foreach (RendererDetail renderer in engine.RendererDetails)
                    {
                        if (activeRendererId.Length == 0 ||
                            String.Equals(renderer.RendererId, activeRendererId,
                                StringComparison.OrdinalIgnoreCase))
                        {
                            activeRendererId = renderer.RendererId;
                            activeRendererName = renderer.FriendlyName;
                            break;
                        }
                    }
                    Console.Out.WriteLine("READY\t" + activeRendererId + "\t" +
                        activeRendererName.Replace('\t', ' ').Replace('\r', ' ')
                            .Replace('\n', ' '));
                    Console.Out.Flush();
                    float ambienceMaster = 1.0f;
                    // 3D listener/emitter pair: XACT applies the authored RPC
                    // distance curves and stereo panning from Sounds.xgs.
                    var listener = new AudioListener();
                    var emitter = new AudioEmitter();
                    var liveCues = new System.Collections.Generic.List<Cue>();
                    var loopingCues = new System.Collections.Generic.Dictionary<string, Cue>(
                        StringComparer.Ordinal);
                    var enemyVoiceCues = new System.Collections.Generic.Dictionary<int, Cue>();
                    var enemyVoiceNames =
                        new System.Collections.Generic.Dictionary<int, string>();
                    var enemyVoiceEmitters =
                        new System.Collections.Generic.Dictionary<int, AudioEmitter>();
                    var enemyVoiceActive =
                        new System.Collections.Generic.Dictionary<int, bool>();
                    var fireballCues = new System.Collections.Generic.Dictionary<int, Cue>();
                    var fireballNames =
                        new System.Collections.Generic.Dictionary<int, string>();
                    var fireballEmitters =
                        new System.Collections.Generic.Dictionary<int, AudioEmitter>();
                    string[] ambientCueNames = { "Birds", "Crickets", "Drips", "lostSouls" };
                    Cue?[] ambientCues = new Cue?[4];
                    var pendingSongs = new System.Collections.Generic.List<string>();
                    Func<string, int> musicPriority = song =>
                    {
                        if (song.StartsWith("Song", StringComparison.Ordinal) &&
                            int.TryParse(song.Substring(4), out int number))
                            return number;
                        // InGameHUD updates all distance triggers before its crater trigger.
                        return 0;
                    };
                    Action<string> queueMusic = song =>
                    {
                        if (pendingSongs.Contains(song))
                            return;
                        pendingSongs.Add(song);
                        pendingSongs.Sort((left, right) =>
                            musicPriority(right).CompareTo(musicPriority(left)));
                    };
                    Cue? musicCue = null;
                    Action<string> startMusic = cueName =>
                    {
                        if (musicCue != null)
                        {
                            if (!musicCue.IsStopped)
                                musicCue.Stop(AudioStopOptions.Immediate);
                            musicCue.Dispose();
                        }
                        musicCue = soundBank.GetCue(cueName);
                        musicCue.Play();
                    };
                    var commands = new BlockingCollection<string>();
                    var reader = new Thread(() =>
                    {
                        string? line;
                        while ((line = Console.In.ReadLine()) != null)
                        {
                            commands.Add(line);
                            if (line == "quit")
                                break;
                        }
                        commands.CompleteAdding();
                    });
                    reader.IsBackground = true;
                    reader.Start();

                    bool running = true;
                    while (running)
                    {
                        while (commands.TryTake(out string command))
                        {
                            if (trace)
                                Console.Error.WriteLine("audio command: " + command);
                            if (command == "quit")
                            {
                                running = false;
                                break;
                            }
                            try
                            {
                                if (command.StartsWith("volume ", StringComparison.Ordinal))
                                {
                                    string[] fields = command.Split(new[] { ' ' },
                                        StringSplitOptions.RemoveEmptyEntries);
                                    if (fields.Length == 4 &&
                                        float.TryParse(fields[1], NumberStyles.Float,
                                            CultureInfo.InvariantCulture, out float effectsLevel) &&
                                        float.TryParse(fields[2], NumberStyles.Float,
                                            CultureInfo.InvariantCulture, out float musicLevel) &&
                                        float.TryParse(fields[3], NumberStyles.Float,
                                            CultureInfo.InvariantCulture, out float ambienceLevel))
                                    {
                                        effects.SetVolume(effectsLevel);
                                        music.SetVolume(musicLevel);
                                        ambienceMaster = ambienceLevel;
                                        foreach (AudioCategory category in ambience)
                                            category.SetVolume(ambienceLevel);
                                    }
                                }
                                else if (command == "music start")
                                {
                                    pendingSongs.Clear();
                                    startMusic("Theme");
                                }
                                else if (command == "music stop")
                                {
                                    pendingSongs.Clear();
                                    if (musicCue != null)
                                    {
                                        if (!musicCue.IsStopped)
                                            musicCue.Stop(AudioStopOptions.Immediate);
                                        musicCue.Dispose();
                                        musicCue = null;
                                    }
                                }
                                else if (command.StartsWith("song ", StringComparison.Ordinal))
                                {
                                    // Vanilla checks transition triggers from Song6 down to Song1,
                                    // then checks SpaceTheme. If several become eligible while a
                                    // cue is playing, the highest-distance track wins next.
                                    string song = command.Substring(5).Trim();
                                    if (musicCue != null && !musicCue.IsStopped)
                                        queueMusic(song);
                                    else
                                        startMusic(song);
                                }
                                else if (command.StartsWith("loop start ", StringComparison.Ordinal))
                                {
                                    string cueName = command.Substring(11).Trim();
                                    if (cueName.Length > 0 &&
                                        (!loopingCues.TryGetValue(cueName, out Cue? cue) ||
                                         cue.IsStopped))
                                    {
                                        if (cue != null)
                                            cue.Dispose();
                                        cue = soundBank.GetCue(cueName);
                                        cue.Play();
                                        loopingCues[cueName] = cue;
                                    }
                                }
                                else if (command.StartsWith("loop stop ", StringComparison.Ordinal))
                                {
                                    string cueName = command.Substring(10).Trim();
                                    if (loopingCues.TryGetValue(cueName, out Cue? cue))
                                    {
                                        if (!cue.IsStopped)
                                            cue.Stop(AudioStopOptions.Immediate);
                                        cue.Dispose();
                                        loopingCues.Remove(cueName);
                                    }
                                }
                                else if (command.StartsWith("listener ", StringComparison.Ordinal))
                                {
                                    string[] fields = command.Split(new[] { ' ' },
                                        StringSplitOptions.RemoveEmptyEntries);
                                    if (fields.Length == 7)
                                    {
                                        listener.Position = new Microsoft.Xna.Framework.Vector3(
                                            float.Parse(fields[1], CultureInfo.InvariantCulture),
                                            float.Parse(fields[2], CultureInfo.InvariantCulture),
                                            float.Parse(fields[3], CultureInfo.InvariantCulture));
                                        listener.Forward = new Microsoft.Xna.Framework.Vector3(
                                            float.Parse(fields[4], CultureInfo.InvariantCulture),
                                            float.Parse(fields[5], CultureInfo.InvariantCulture),
                                            float.Parse(fields[6], CultureInfo.InvariantCulture));
                                        listener.Up = Microsoft.Xna.Framework.Vector3.Up;
                                    }
                                }
                                else if (command == "voices begin")
                                {
                                    var ids = new System.Collections.Generic.List<int>(
                                        enemyVoiceActive.Keys);
                                    foreach (int id in ids)
                                        enemyVoiceActive[id] = false;
                                }
                                else if (command == "voices end")
                                {
                                    var ids = new System.Collections.Generic.List<int>(
                                        enemyVoiceCues.Keys);
                                    foreach (int id in ids)
                                    {
                                        Cue cue = enemyVoiceCues[id];
                                        if (!enemyVoiceActive[id] && cue.IsStopped)
                                        {
                                            cue.Dispose();
                                            enemyVoiceCues.Remove(id);
                                            enemyVoiceNames.Remove(id);
                                            enemyVoiceEmitters.Remove(id);
                                            enemyVoiceActive.Remove(id);
                                        }
                                    }
                                }
                                else if (command == "voices clear")
                                {
                                    foreach (Cue cue in enemyVoiceCues.Values)
                                    {
                                        if (!cue.IsStopped)
                                            cue.Stop(AudioStopOptions.Immediate);
                                        cue.Dispose();
                                    }
                                    enemyVoiceCues.Clear();
                                    enemyVoiceNames.Clear();
                                    enemyVoiceEmitters.Clear();
                                    enemyVoiceActive.Clear();
                                }
                                else if (command.StartsWith("voice ", StringComparison.Ordinal))
                                {
                                    string[] fields = command.Split(new[] { ' ' },
                                        StringSplitOptions.RemoveEmptyEntries);
                                    if (fields.Length == 6 &&
                                        int.TryParse(fields[1], NumberStyles.Integer,
                                            CultureInfo.InvariantCulture, out int id))
                                    {
                                        string cueName = fields[2];
                                        if (enemyVoiceNames.TryGetValue(id,
                                                out string? previousName) &&
                                            !String.Equals(previousName, cueName,
                                                StringComparison.Ordinal))
                                        {
                                            Cue previousCue = enemyVoiceCues[id];
                                            if (!previousCue.IsStopped)
                                                previousCue.Stop(AudioStopOptions.Immediate);
                                            previousCue.Dispose();
                                            enemyVoiceCues.Remove(id);
                                        }
                                        if (!enemyVoiceEmitters.TryGetValue(id,
                                                out AudioEmitter? voiceEmitter))
                                        {
                                            voiceEmitter = new AudioEmitter();
                                            enemyVoiceEmitters[id] = voiceEmitter;
                                        }
                                        voiceEmitter.Position =
                                            new Microsoft.Xna.Framework.Vector3(
                                                float.Parse(fields[3],
                                                    CultureInfo.InvariantCulture),
                                                float.Parse(fields[4],
                                                    CultureInfo.InvariantCulture),
                                                float.Parse(fields[5],
                                                    CultureInfo.InvariantCulture));
                                        voiceEmitter.Up = Microsoft.Xna.Framework.Vector3.Up;
                                        enemyVoiceNames[id] = cueName;
                                        enemyVoiceActive[id] = true;
                                        if (!enemyVoiceCues.TryGetValue(id, out Cue? cue) ||
                                            cue.IsStopped)
                                        {
                                            if (cue != null)
                                                cue.Dispose();
                                            cue = soundBank.GetCue(cueName);
                                            cue.Apply3D(listener, voiceEmitter);
                                            cue.Play();
                                            enemyVoiceCues[id] = cue;
                                        }
                                        else
                                        {
                                            cue.Apply3D(listener, voiceEmitter);
                                        }
                                    }
                                }
                                else if (command == "fireballs clear")
                                {
                                    foreach (Cue cue in fireballCues.Values)
                                    {
                                        if (!cue.IsStopped)
                                            cue.Stop(AudioStopOptions.Immediate);
                                        cue.Dispose();
                                    }
                                    fireballCues.Clear();
                                    fireballNames.Clear();
                                    fireballEmitters.Clear();
                                }
                                else if (command.StartsWith("fireball stop ",
                                    StringComparison.Ordinal))
                                {
                                    if (int.TryParse(command.Substring(14),
                                            NumberStyles.Integer, CultureInfo.InvariantCulture,
                                            out int id) && fireballCues.TryGetValue(id,
                                                out Cue? cue))
                                    {
                                        if (!cue.IsStopped)
                                            cue.Stop(AudioStopOptions.Immediate);
                                        cue.Dispose();
                                        fireballCues.Remove(id);
                                        fireballNames.Remove(id);
                                        fireballEmitters.Remove(id);
                                    }
                                }
                                else if (command.StartsWith("fireball ",
                                    StringComparison.Ordinal))
                                {
                                    string[] fields = command.Split(new[] { ' ' },
                                        StringSplitOptions.RemoveEmptyEntries);
                                    if (fields.Length == 9 &&
                                        int.TryParse(fields[1], NumberStyles.Integer,
                                            CultureInfo.InvariantCulture, out int id))
                                    {
                                        string cueName = fields[2];
                                        if (fireballNames.TryGetValue(id,
                                                out string? previousName) &&
                                            !String.Equals(previousName, cueName,
                                                StringComparison.Ordinal))
                                        {
                                            Cue previousCue = fireballCues[id];
                                            if (!previousCue.IsStopped)
                                                previousCue.Stop(AudioStopOptions.Immediate);
                                            previousCue.Dispose();
                                            fireballCues.Remove(id);
                                        }
                                        if (!fireballEmitters.TryGetValue(id,
                                                out AudioEmitter? fireballEmitter))
                                        {
                                            fireballEmitter = new AudioEmitter();
                                            fireballEmitters[id] = fireballEmitter;
                                        }
                                        fireballEmitter.Position =
                                            new Microsoft.Xna.Framework.Vector3(
                                                float.Parse(fields[3],
                                                    CultureInfo.InvariantCulture),
                                                float.Parse(fields[4],
                                                    CultureInfo.InvariantCulture),
                                                float.Parse(fields[5],
                                                    CultureInfo.InvariantCulture));
                                        fireballEmitter.Velocity =
                                            new Microsoft.Xna.Framework.Vector3(
                                                float.Parse(fields[6],
                                                    CultureInfo.InvariantCulture),
                                                float.Parse(fields[7],
                                                    CultureInfo.InvariantCulture),
                                                float.Parse(fields[8],
                                                    CultureInfo.InvariantCulture));
                                        fireballEmitter.Up = Microsoft.Xna.Framework.Vector3.Up;
                                        fireballNames[id] = cueName;
                                        if (!fireballCues.TryGetValue(id, out Cue? cue))
                                        {
                                            cue = soundBank.GetCue(cueName);
                                            cue.Apply3D(listener, fireballEmitter);
                                            cue.Play();
                                            fireballCues[id] = cue;
                                        }
                                        else if (!cue.IsStopped)
                                        {
                                            cue.Apply3D(listener, fireballEmitter);
                                        }
                                    }
                                }
                                else if (command.StartsWith("cue3d ", StringComparison.Ordinal))
                                {
                                    string[] fields = command.Split(new[] { ' ' },
                                        StringSplitOptions.RemoveEmptyEntries);
                                    if (fields.Length == 5)
                                    {
                                        emitter.Position = new Microsoft.Xna.Framework.Vector3(
                                            float.Parse(fields[2], CultureInfo.InvariantCulture),
                                            float.Parse(fields[3], CultureInfo.InvariantCulture),
                                            float.Parse(fields[4], CultureInfo.InvariantCulture));
                                        Cue cue = soundBank.GetCue(fields[1]);
                                        cue.Apply3D(listener, emitter);
                                        cue.Play();
                                        liveCues.Add(cue);
                                    }
                                }
                                else if (command.StartsWith("ambient ", StringComparison.Ordinal))
                                {
                                    string[] fields = command.Split(new[] { ' ' },
                                        StringSplitOptions.RemoveEmptyEntries);
                                    if (fields.Length == 5)
                                    {
                                        for (int i = 0; i < 4; i++)
                                        {
                                            if (ambientCues[i] == null || ambientCues[i]!.IsStopped)
                                            {
                                                ambientCues[i] = soundBank.GetCue(ambientCueNames[i]);
                                                ambientCues[i]!.Play();
                                            }
                                            float weight = float.Parse(fields[1 + i],
                                                CultureInfo.InvariantCulture);
                                            ambience[i].SetVolume(Math.Max(0f,
                                                Math.Min(1f, weight)) * ambienceMaster);
                                        }
                                    }
                                }
                                else if (command.StartsWith("outdoors ", StringComparison.Ordinal))
                                {
                                    // Drives the authored reverb morph: 1 = dry
                                    // surface, 0 = cave/hell reverb preset.
                                    engine.SetGlobalVariable("Outdoors", float.Parse(
                                        command.Substring(9), CultureInfo.InvariantCulture));
                                }
                                else
                                {
                                    soundBank.PlayCue(command);
                                }
                            }
                            catch (Exception error)
                            {
                                Console.Error.WriteLine("command " + command + ": " + error);
                            }
                        }
                        // The game owns this helper through the stdin pipe. A normal shutdown
                        // sends `quit`, but a crash or forced termination only closes the pipe;
                        // BlockingCollection becomes completed after the final queued command.
                        // Treat that EOF as quit so an audio-only process can never be orphaned.
                        if (commands.IsCompleted)
                            running = false;
                        engine.Update();
                        if (musicCue != null && musicCue.IsStopped)
                        {
                            musicCue.Dispose();
                            musicCue = null;
                            if (pendingSongs.Count > 0)
                            {
                                string nextSong = pendingSongs[0];
                                pendingSongs.RemoveAt(0);
                                startMusic(nextSong);
                            }
                        }
                        liveCues.RemoveAll(cue =>
                        {
                            if (cue.IsStopped)
                            {
                                cue.Dispose();
                                return true;
                            }
                            return false;
                        });
                        var voiceIds = new System.Collections.Generic.List<int>(
                            enemyVoiceCues.Keys);
                        foreach (int id in voiceIds)
                        {
                            Cue cue = enemyVoiceCues[id];
                            if (cue.IsStopped && enemyVoiceActive[id])
                            {
                                cue.Dispose();
                                cue = soundBank.GetCue(enemyVoiceNames[id]);
                                cue.Apply3D(listener, enemyVoiceEmitters[id]);
                                cue.Play();
                                enemyVoiceCues[id] = cue;
                            }
                            else if (cue.IsStopped)
                            {
                                cue.Dispose();
                                enemyVoiceCues.Remove(id);
                                enemyVoiceNames.Remove(id);
                                enemyVoiceEmitters.Remove(id);
                                enemyVoiceActive.Remove(id);
                            }
                        }
                        Thread.Sleep(8);
                    }
                    if (musicCue != null)
                    {
                        if (!musicCue.IsStopped)
                            musicCue.Stop(AudioStopOptions.Immediate);
                        musicCue.Dispose();
                    }
                    foreach (Cue? cue in ambientCues)
                    {
                        if (cue == null)
                            continue;
                        cue.Stop(AudioStopOptions.Immediate);
                        cue.Dispose();
                    }
                    foreach (Cue cue in liveCues)
                    {
                        cue.Stop(AudioStopOptions.Immediate);
                        cue.Dispose();
                    }
                    foreach (Cue cue in loopingCues.Values)
                    {
                        cue.Stop(AudioStopOptions.Immediate);
                        cue.Dispose();
                    }
                    foreach (Cue cue in enemyVoiceCues.Values)
                    {
                        if (!cue.IsStopped)
                            cue.Stop(AudioStopOptions.Immediate);
                        cue.Dispose();
                    }
                    foreach (Cue cue in fireballCues.Values)
                    {
                        if (!cue.IsStopped)
                            cue.Stop(AudioStopOptions.Immediate);
                        cue.Dispose();
                    }
                }
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine(error.ToString());
                return 1;
            }
        }
    }
}
