using System;
using System.Globalization;
using System.IO;
using System.Reflection;

namespace OpenMinerZ.OriginalParityProbe
{
    internal static class Program
    {
        private static void Main(string[] args)
        {
            string gameDirectory = args.Length > 0
                ? Path.GetFullPath(args[0])
                : @"C:\Program Files (x86)\Steam\steamapps\common\CastleMiner Z";
            Assembly original =
                Assembly.Load(File.ReadAllBytes(Path.Combine(gameDirectory, "DNA.Common.dll")));
            Type perlinType = original.GetType("DNA.Drawing.Noise.PerlinNoise", true);
            Type integerType = original.GetType("DNA.Drawing.Noise.IntNoise", true);
            object perlin = Activator.CreateInstance(perlinType, new object[] { new Random(1337) });
            object integer = Activator.CreateInstance(integerType, new object[] { new Random(1337) });
            MethodInfo perlin2 =
                perlinType.GetMethod("ComputeNoise", new[] { typeof(float), typeof(float) });
            MethodInfo integer3 =
                integerType.GetMethod("ComputeNoise", new[] { typeof(int), typeof(int), typeof(int) });
            float[,] samples =
            {
                { 0.1f, 0.2f },
                { 1.25f, -2.5f },
                { 9.375f, 4.125f },
                { 0.0f, 0.0f },
            };
            for (int index = 0; index < samples.GetLength(0); ++index)
            {
                float value = (float)perlin2.Invoke(
                    perlin, new object[] { samples[index, 0], samples[index, 1] });
                Console.WriteLine(value.ToString("R", CultureInfo.InvariantCulture));
            }
            Console.WriteLine(integer3.Invoke(integer, new object[] { 4, -9, 17 }));
            Console.WriteLine(integer3.Invoke(integer, new object[] { 1000, 1000, 1000 }));
        }
    }
}
