param(
    [string]$DatasetRoot = (Join-Path $PSScriptRoot '..\dataset')
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

public static class VoiceCommandSplitter
{
    public sealed class SegmentInfo
    {
        public string FileName;
        public double StartSeconds;
        public double EndSeconds;
        public double DurationSeconds;
        public double PeakRms;
    }

    private static short[] ReadPcm16Mono(string path, out int sampleRate)
    {
        using (var stream = File.OpenRead(path))
        using (var reader = new BinaryReader(stream))
        {
            if (new string(reader.ReadChars(4)) != "RIFF") throw new InvalidDataException("Missing RIFF");
            reader.ReadUInt32();
            if (new string(reader.ReadChars(4)) != "WAVE") throw new InvalidDataException("Missing WAVE");
            ushort format = 0, channels = 0, bits = 0;
            int rate = 0;
            byte[] pcm = null;
            while (stream.Position + 8 <= stream.Length)
            {
                string id = new string(reader.ReadChars(4));
                int size = reader.ReadInt32();
                long next = stream.Position + size + (size & 1);
                if (id == "fmt ")
                {
                    format = reader.ReadUInt16();
                    channels = reader.ReadUInt16();
                    rate = reader.ReadInt32();
                    reader.ReadInt32(); reader.ReadUInt16();
                    bits = reader.ReadUInt16();
                }
                else if (id == "data") pcm = reader.ReadBytes(size);
                stream.Position = next;
            }
            if (format != 1 || channels != 1 || bits != 16 || rate != 16000 || pcm == null)
                throw new InvalidDataException("Expected PCM 16 kHz mono 16-bit WAV");
            var samples = new short[pcm.Length / 2];
            Buffer.BlockCopy(pcm, 0, samples, 0, pcm.Length);
            sampleRate = rate;
            return samples;
        }
    }

    private static void WriteWav(string path, short[] samples, int start, int count, int rate)
    {
        using (var writer = new BinaryWriter(File.Create(path)))
        {
            int bytes = count * 2;
            writer.Write(System.Text.Encoding.ASCII.GetBytes("RIFF"));
            writer.Write(36 + bytes);
            writer.Write(System.Text.Encoding.ASCII.GetBytes("WAVEfmt "));
            writer.Write(16); writer.Write((ushort)1); writer.Write((ushort)1);
            writer.Write(rate); writer.Write(rate * 2); writer.Write((ushort)2); writer.Write((ushort)16);
            writer.Write(System.Text.Encoding.ASCII.GetBytes("data")); writer.Write(bytes);
            var buffer = new byte[bytes];
            Buffer.BlockCopy(samples, start * 2, buffer, 0, bytes);
            writer.Write(buffer);
        }
    }

    public static SegmentInfo[] Split(string input, string outputDir, string prefix)
    {
        int rate;
        short[] samples = ReadPcm16Mono(input, out rate);
        int frame = rate / 50; // 20 ms
        int frames = (samples.Length + frame - 1) / frame;
        var rms = new double[frames];
        for (int f = 0; f < frames; f++)
        {
            int start = f * frame, end = Math.Min(start + frame, samples.Length);
            double sum = 0;
            for (int i = start; i < end; i++) { double s = samples[i]; sum += s * s; }
            rms[f] = Math.Sqrt(sum / Math.Max(1, end - start));
        }
        var sorted = (double[])rms.Clone(); Array.Sort(sorted);
        double noise = sorted[(int)(sorted.Length * 0.20)];
        double peak = sorted[sorted.Length - 1];
        double threshold = Math.Max(250.0, Math.Max(noise * 3.0, peak * 0.07));

        int hangover = 12; // merge gaps up to 240 ms
        int pad = 8;       // 160 ms at both ends
        int minFrames = 12; // 240 ms
        var raw = new List<Tuple<int,int>>();
        int activeStart = -1, lastActive = -1;
        for (int f = 0; f < frames; f++)
        {
            if (rms[f] >= threshold)
            {
                if (activeStart < 0) activeStart = f;
                lastActive = f;
            }
            else if (activeStart >= 0 && f - lastActive > hangover)
            {
                if (lastActive - activeStart + 1 >= minFrames)
                    raw.Add(Tuple.Create(Math.Max(0, activeStart - pad), Math.Min(frames - 1, lastActive + pad)));
                activeStart = lastActive = -1;
            }
        }
        if (activeStart >= 0 && lastActive - activeStart + 1 >= minFrames)
            raw.Add(Tuple.Create(Math.Max(0, activeStart - pad), Math.Min(frames - 1, lastActive + pad)));

        Directory.CreateDirectory(outputDir);
        foreach (string old in Directory.GetFiles(outputDir, prefix + "_*.wav")) File.Delete(old);
        var result = new List<SegmentInfo>();
        int index = 1;
        foreach (var seg in raw)
        {
            int startSample = seg.Item1 * frame;
            int endSample = Math.Min(samples.Length, (seg.Item2 + 1) * frame);
            double duration = (endSample - startSample) / (double)rate;
            if (duration > 3.0) continue; // likely continuous noise, not one command
            string fileName = prefix + "_" + index.ToString("D3") + ".wav";
            WriteWav(Path.Combine(outputDir, fileName), samples, startSample, endSample - startSample, rate);
            double segmentPeak = 0;
            for (int f = seg.Item1; f <= seg.Item2; f++) segmentPeak = Math.Max(segmentPeak, rms[f]);
            result.Add(new SegmentInfo {
                FileName = fileName,
                StartSeconds = Math.Round(startSample / (double)rate, 3),
                EndSeconds = Math.Round(endSample / (double)rate, 3),
                DurationSeconds = Math.Round(duration, 3),
                PeakRms = Math.Round(segmentPeak, 1)
            });
            index++;
        }
        return result.ToArray();
    }
}
'@

$wavRoot = Join-Path $DatasetRoot 'wav'
$sampleRoot = Join-Path $DatasetRoot 'samples'
$leftRoot = Join-Path $sampleRoot 'left'
$rightRoot = Join-Path $sampleRoot 'right'
$left = [VoiceCommandSplitter]::Split((Join-Path $wavRoot 'left_source.wav'), $leftRoot, 'left')
$right = [VoiceCommandSplitter]::Split((Join-Path $wavRoot 'right_source.wav'), $rightRoot, 'right')

$manifest = @()
$manifest += $left | ForEach-Object { [PSCustomObject]@{ label='left'; file=('samples/left/' + $_.FileName); start_s=$_.StartSeconds; end_s=$_.EndSeconds; duration_s=$_.DurationSeconds; peak_rms=$_.PeakRms } }
$manifest += $right | ForEach-Object { [PSCustomObject]@{ label='right'; file=('samples/right/' + $_.FileName); start_s=$_.StartSeconds; end_s=$_.EndSeconds; duration_s=$_.DurationSeconds; peak_rms=$_.PeakRms } }
$manifest | Export-Csv -LiteralPath (Join-Path $DatasetRoot 'manifest.csv') -NoTypeInformation -Encoding UTF8

[PSCustomObject]@{
    LeftSamples = $left.Count
    RightSamples = $right.Count
    LeftDurationRange = if ($left.Count) { '{0:N3}-{1:N3}' -f (($left.DurationSeconds | Measure-Object -Minimum).Minimum), (($left.DurationSeconds | Measure-Object -Maximum).Maximum) } else { 'none' }
    RightDurationRange = if ($right.Count) { '{0:N3}-{1:N3}' -f (($right.DurationSeconds | Measure-Object -Minimum).Minimum), (($right.DurationSeconds | Measure-Object -Maximum).Maximum) } else { 'none' }
}
