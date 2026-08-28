using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;

namespace OpenMinerZ.ContentBridge
{
    internal readonly struct Pixel
    {
        public Pixel(byte red, byte green, byte blue, byte alpha)
        {
            Red = red;
            Green = green;
            Blue = blue;
            Alpha = alpha;
        }

        public byte Red { get; }
        public byte Green { get; }
        public byte Blue { get; }
        public byte Alpha { get; }
    }

    internal static class TextureDecoder
    {
        public static Pixel[] Read(Texture2D texture)
        {
            byte[] data = new byte[DataSize(texture.Width, texture.Height, texture.Format)];
            texture.GetData(data);
            return Decode(data, texture.Width, texture.Height, texture.Format);
        }

        public static Pixel[] Read(TextureCube texture, CubeMapFace face)
        {
            byte[] data = new byte[DataSize(texture.Size, texture.Size, texture.Format)];
            texture.GetData(face, data);
            return Decode(data, texture.Size, texture.Size, texture.Format);
        }

        private static int DataSize(int width, int height, SurfaceFormat format)
        {
            switch (format)
            {
                case SurfaceFormat.Color:
                    return width * height * 4;
                case SurfaceFormat.Dxt1:
                    return ((width + 3) / 4) * ((height + 3) / 4) * 8;
                case SurfaceFormat.Dxt3:
                case SurfaceFormat.Dxt5:
                    return ((width + 3) / 4) * ((height + 3) / 4) * 16;
                default:
                    throw new NotSupportedException($"Unsupported original texture format: {format}");
            }
        }

        private static Pixel[] Decode(byte[] source, int width, int height, SurfaceFormat format)
        {
            if (format == SurfaceFormat.Color)
            {
                Pixel[] raw = new Pixel[width * height];
                for (int index = 0; index < raw.Length; ++index)
                {
                    int offset = index * 4;
                    raw[index] = new Pixel(source[offset], source[offset + 1], source[offset + 2],
                                           source[offset + 3]);
                }
                return raw;
            }

            Pixel[] result = new Pixel[width * height];
            int blockBytes = format == SurfaceFormat.Dxt1 ? 8 : 16;
            int offsetBytes = 0;
            for (int blockY = 0; blockY < (height + 3) / 4; ++blockY)
            {
                for (int blockX = 0; blockX < (width + 3) / 4; ++blockX)
                {
                    DecodeBlock(source, offsetBytes, result, width, height, blockX * 4, blockY * 4,
                                format);
                    offsetBytes += blockBytes;
                }
            }
            return result;
        }

        private static void DecodeBlock(byte[] source, int offset, Pixel[] output, int width,
                                        int height, int targetX, int targetY, SurfaceFormat format)
        {
            int colorOffset = format == SurfaceFormat.Dxt1 ? offset : offset + 8;
            ushort color0 = ReadUInt16(source, colorOffset);
            ushort color1 = ReadUInt16(source, colorOffset + 2);
            Pixel[] palette = BuildPalette(color0, color1, format == SurfaceFormat.Dxt1);
            uint colorIndices = ReadUInt32(source, colorOffset + 4);
            byte[] alpha = ReadAlpha(source, offset, format);
            for (int pixel = 0; pixel < 16; ++pixel)
            {
                int x = targetX + pixel % 4;
                int y = targetY + pixel / 4;
                if (x >= width || y >= height)
                {
                    continue;
                }
                int paletteIndex = (int)((colorIndices >> (pixel * 2)) & 3U);
                Pixel color = palette[paletteIndex];
                byte finalAlpha = format == SurfaceFormat.Dxt1 ? color.Alpha : alpha[pixel];
                output[x + y * width] = new Pixel(color.Red, color.Green, color.Blue, finalAlpha);
            }
        }

        private static Pixel[] BuildPalette(ushort packed0, ushort packed1, bool dxt1)
        {
            Pixel first = From565(packed0);
            Pixel second = From565(packed1);
            Pixel[] result = new Pixel[4];
            result[0] = first;
            result[1] = second;
            if (!dxt1 || packed0 > packed1)
            {
                result[2] = Blend(first, second, 2, 1, 3);
                result[3] = Blend(first, second, 1, 2, 3);
            }
            else
            {
                result[2] = Blend(first, second, 1, 1, 2);
                result[3] = new Pixel(0, 0, 0, 0);
            }
            return result;
        }

        private static byte[] ReadAlpha(byte[] source, int offset, SurfaceFormat format)
        {
            byte[] result = new byte[16];
            if (format == SurfaceFormat.Dxt1)
            {
                for (int index = 0; index < result.Length; ++index)
                {
                    result[index] = 255;
                }
                return result;
            }
            if (format == SurfaceFormat.Dxt3)
            {
                ulong bits = ReadUInt64(source, offset);
                for (int index = 0; index < result.Length; ++index)
                {
                    result[index] = (byte)(((bits >> (index * 4)) & 15UL) * 17UL);
                }
                return result;
            }

            byte alpha0 = source[offset];
            byte alpha1 = source[offset + 1];
            byte[] palette = new byte[8];
            palette[0] = alpha0;
            palette[1] = alpha1;
            if (alpha0 > alpha1)
            {
                for (int index = 1; index <= 6; ++index)
                {
                    palette[index + 1] = (byte)(((7 - index) * alpha0 + index * alpha1) / 7);
                }
            }
            else
            {
                for (int index = 1; index <= 4; ++index)
                {
                    palette[index + 1] = (byte)(((5 - index) * alpha0 + index * alpha1) / 5);
                }
                palette[6] = 0;
                palette[7] = 255;
            }
            ulong indices = 0;
            for (int byteIndex = 0; byteIndex < 6; ++byteIndex)
            {
                indices |= (ulong)source[offset + 2 + byteIndex] << (byteIndex * 8);
            }
            for (int index = 0; index < result.Length; ++index)
            {
                result[index] = palette[(indices >> (index * 3)) & 7UL];
            }
            return result;
        }

        private static Pixel From565(ushort packed)
        {
            int red = (packed >> 11) & 31;
            int green = (packed >> 5) & 63;
            int blue = packed & 31;
            return new Pixel((byte)((red << 3) | (red >> 2)),
                             (byte)((green << 2) | (green >> 4)),
                             (byte)((blue << 3) | (blue >> 2)), 255);
        }

        private static Pixel Blend(Pixel first, Pixel second, int firstWeight, int secondWeight,
                                   int divisor)
        {
            return new Pixel((byte)((first.Red * firstWeight + second.Red * secondWeight) / divisor),
                             (byte)((first.Green * firstWeight + second.Green * secondWeight) / divisor),
                             (byte)((first.Blue * firstWeight + second.Blue * secondWeight) / divisor),
                             255);
        }

        private static ushort ReadUInt16(byte[] source, int offset) =>
            (ushort)(source[offset] | source[offset + 1] << 8);

        private static uint ReadUInt32(byte[] source, int offset) =>
            (uint)(source[offset] | source[offset + 1] << 8 | source[offset + 2] << 16 |
                   source[offset + 3] << 24);

        private static ulong ReadUInt64(byte[] source, int offset)
        {
            ulong result = 0;
            for (int index = 0; index < 8; ++index)
            {
                result |= (ulong)source[offset + index] << (index * 8);
            }
            return result;
        }
    }

    internal static class BitmapWriter
    {
        public static void Write(string path, Pixel[] pixels, int width, int height)
        {
            using (FileStream stream = File.Create(path))
            using (BinaryWriter writer = new BinaryWriter(stream))
            {
                int pixelBytes = width * height * 4;
                const int bitmapV4HeaderBytes = 108;
                writer.Write((byte)'B');
                writer.Write((byte)'M');
                writer.Write(14 + bitmapV4HeaderBytes + pixelBytes);
                writer.Write(0);
                writer.Write(14 + bitmapV4HeaderBytes);
                // BITMAPV4HEADER explicitly declares BGRA channel masks. A
                // 32-bit BI_RGB header treats its fourth byte as unused on
                // Windows/SDL, which silently turned every exported alpha
                // channel into zero on the D3D11 texture.
                writer.Write(bitmapV4HeaderBytes);
                writer.Write(width);
                writer.Write(height);
                writer.Write((ushort)1);
                writer.Write((ushort)32);
                writer.Write(3); // BI_BITFIELDS
                writer.Write(pixelBytes);
                writer.Write(2835);
                writer.Write(2835);
                writer.Write(0);
                writer.Write(0);
                writer.Write(0x00FF0000U); // red
                writer.Write(0x0000FF00U); // green
                writer.Write(0x000000FFU); // blue
                writer.Write(0xFF000000U); // alpha
                writer.Write(0x73524742U); // LCS_sRGB
                for (int headerByte = 60; headerByte < bitmapV4HeaderBytes;
                     headerByte += sizeof(uint))
                {
                    writer.Write(0U);
                }
                for (int y = height - 1; y >= 0; --y)
                {
                    for (int x = 0; x < width; ++x)
                    {
                        Pixel pixel = pixels[x + y * width];
                        writer.Write(pixel.Blue);
                        writer.Write(pixel.Green);
                        writer.Write(pixel.Red);
                        writer.Write(pixel.Alpha);
                    }
                }
            }
        }
    }

    internal sealed class BridgeGame : Game
    {
        private readonly GraphicsDeviceManager graphics;
        private readonly string outputDirectory;
        private readonly bool refreshedLayout;
        private Exception? failure;

        public BridgeGame(string contentDirectory, string outputDirectory)
        {
            this.outputDirectory = outputDirectory;
            refreshedLayout = File.Exists(Path.Combine(
                contentDirectory, "ReachContent", "Terrain", "Textures.xnb"));
            graphics = new GraphicsDeviceManager(this)
            {
                PreferredBackBufferWidth = 16,
                PreferredBackBufferHeight = 16,
                SynchronizeWithVerticalRetrace = false,
                GraphicsProfile = GraphicsProfile.Reach,
            };
            Content.RootDirectory = contentDirectory;
            Window.Title = "OpenMinerZ Content Bridge";
            IsFixedTimeStep = false;
        }

        public void ThrowIfFailed()
        {
            if (failure != null)
            {
                throw new InvalidOperationException("Content extraction failed", failure);
            }
        }

        protected override void LoadContent()
        {
            try
            {
                Directory.CreateDirectory(outputDirectory);
                Texture[] terrain = Content.Load<Texture[]>(Asset(
                    @"Terrain\Textures", @"ReachContent\Terrain\Textures"));
                string[] terrainNames = { "terrain_diffuse.bmp", "terrain_normal_spec.bmp",
                                          "terrain_metal_light.bmp" };
                for (int index = 0; index < terrainNames.Length; ++index)
                {
                    Texture2D texture = terrain[index] as Texture2D ??
                                        throw new InvalidDataException("Terrain texture was not 2D");
                    Save(texture, terrainNames[index]);
                }

                // CrackBox samples this original progress texture over the selected voxel.
                // Keep it in the same local derived-data cache as the terrain sheets.
                Save(Content.Load<Texture2D>("CracksAll_0"), "block_cracks.bmp");

                // These scoped-weapon HUD sprites are stored inside DNA's SpriteManager,
                // not as independent Texture2D assets. Crop the authored source rectangles
                // from the installed sheet so the native client can draw the exact art.
                object uiSprites = Content.Load<object>(Asset("SpriteSheet", @"UI\SpriteSheet"));
                SaveSprite(uiSprites, "SniperScope", "sniper_scope.bmp");
                SaveSprite(uiSprites, "MissleLock", "missile_lock.bmp");
                SaveSprite(uiSprites, "MissleLocking", "missile_locking.bmp");

                Model selector = Content.Load<Model>("Selector");
                int selectorTexture = 0;
                foreach (ModelMesh mesh in selector.Meshes)
                {
                    foreach (Effect effect in mesh.Effects)
                    {
                        foreach (EffectParameter parameter in effect.Parameters)
                        {
                            if (parameter.ParameterType == EffectParameterType.Texture2D)
                            {
                                Texture2D texture = parameter.GetValueTexture2D();
                                if (texture != null)
                                {
                                    Save(texture, $"selector_{selectorTexture++}.bmp");
                                }
                            }
                        }
                    }
                }

                var skies = new Dictionary<string, string>
                {
                    [Asset(@"Textures\Skys\ClearSky", @"ReachContent\Textures\Skys\ClearSky")] = "sky_clear",
                    [Asset(@"Textures\Skys\DawnSky", @"ReachContent\Textures\Skys\DawnSky")] = "sky_dawn",
                    [Asset(@"Textures\Skys\NightSky", @"ReachContent\Textures\Skys\NightSky")] = "sky_night",
                    [Asset(@"Textures\Skys\SunSet", @"ReachContent\Textures\Skys\SunSet")] = "sky_sunset",
                };
                foreach (KeyValuePair<string, string> sky in skies)
                {
                    TextureCube cube = Content.Load<TextureCube>(sky.Key);
                    foreach (CubeMapFace face in Enum.GetValues(typeof(CubeMapFace)))
                    {
                        Pixel[] pixels = TextureDecoder.Read(cube, face);
                        BitmapWriter.Write(Path.Combine(outputDirectory,
                                                       $"{sky.Value}_{face.ToString().ToLowerInvariant()}.bmp"),
                                           pixels, cube.Size, cube.Size);
                    }
                }

                ExportAllAnimations();
                ExportParticle(Asset("SmokeEffect", @"ParticleEffects\SmokeEffect"), "mining_smoke");
                ExportParticle(Asset("SparksEffect", @"ParticleEffects\SparksEffect"), "mining_sparks");
                ExportParticle(Asset("RocksEffect", @"ParticleEffects\RocksEffect"), "mining_rocks");
                // Placed-torch VFX (TorchCloud) and the shared explosion/fireball set.
                ExportParticle(Asset("TorchFire", @"ParticleEffects\TorchFire"), "torch_fire");
                ExportParticle(Asset("TorchSmoke", @"ParticleEffects\TorchSmoke"), "torch_smoke");
                ExportParticle(Asset("FireGlow", @"ParticleEffects\FireGlow"), "fire_glow");
                ExportParticle(Asset("FlashEffect", @"ParticleEffects\FlashEffect"), "flash_effect");
                ExportParticle(Asset("IceFlash", @"ParticleEffects\IceFlash"), "ice_flash");
                ExportParticle(Asset("BlasterFlash", @"ParticleEffects\BlasterFlash"), "blaster_flash");
                ExportParticle(Asset("FirePuff", @"ParticleEffects\FirePuff"), "fire_puff");
                ExportParticle(Asset("IcePuff", @"ParticleEffects\IcePuff"), "ice_puff");
                ExportParticle(Asset("BigSmokePuff", @"ParticleEffects\BigSmokePuff"), "big_smoke_puff");
                ExportParticle(Asset("BigRockBlast", @"ParticleEffects\BigRockBlast"), "big_rock_blast");
                ExportParticle(Asset("FireTrail", @"ParticleEffects\FireTrail"), "fire_trail");
                ExportParticle(Asset("SmokeTrail", @"ParticleEffects\SmokeTrail"), "smoke_trail");
                ExportParticle(Asset("RocketFireTrail", @"ParticleEffects\RocketFireTrail"), "rocket_fire_trail");
                ExportParticle(Asset("RocketSmokeTrail", @"ParticleEffects\RocketSmokeTrail"), "rocket_smoke_trail");
                ExportParticle(Asset("FireBallEffect", @"ParticleEffects\FireBallEffect"), "fireball_effect");
                ExportStaticModel(Asset("PickAxe", @"ReachContent\Props\Tools\PickAxe\Model"), "pickaxe.omsm");
                ExportStaticModel(Asset("Compass", @"ReachContent\Props\Tools\Compass\Model"), "compass.omsm");
                ExportStaticModel(Asset("Colt", @"ReachContent\Props\Weapons\Conventional\Pistol\Model"), "pistol.omsm");
                ExportStaticModel(Asset("Knife", @"ReachContent\Props\Weapons\Conventional\Knife\Model"), "knife.omsm");
                ExportStaticModel(Asset("Ammo", @"ReachContent\Props\Items\Ammo\Model"), "ammo.omsm");
                ExportStaticModel(Asset("Torch", @"ReachContent\Props\Items\Torch\Model"), "torch.omsm");
                ExportStaticModel(Asset("Ore", @"ReachContent\Props\Items\Ore\Model"), "ore.omsm");
                ExportStaticModel(Asset("Gems", @"ReachContent\Props\Items\Gems\Model"), "gems.omsm");
                ExportStaticModel(Asset("Bars", @"ReachContent\Props\Items\Bars\Model"), "bars.omsm");
                // Classic stores the original wood door as one root asset. The
                // refreshed build moved it into the item-model hierarchy and
                // added three model-identical strong-door voxel families.
                if (refreshedLayout)
                {
                    ExportStaticModel(@"ReachContent\Props\Items\Door\Model", "door_wood.omsm");
                    ExportStaticModel(@"ReachContent\Props\Items\IronDoor\Model", "door_iron.omsm");
                    ExportStaticModel(@"ReachContent\Props\Items\Diamond\Door\Model", "door_diamond.omsm");
                    ExportStaticModel(@"ReachContent\Props\Items\TechDoor\Model", "door_tech.omsm");
                }
                else
                {
                    ExportStaticModel("WoodDoor", "door_wood.omsm");
                }
                // Held weapon/tool models referenced by the original inventory classes.
                ExportStaticModel(Asset("AK", @"ReachContent\Props\Weapons\Conventional\AR\Model"), "ak.omsm");
                ExportStaticModel(Asset("PumpShotgun", @"ReachContent\Props\Weapons\Conventional\Shotgun\Model"), "shotgun.omsm");
                ExportStaticModel(Asset("BoltRifle", @"ReachContent\Props\Weapons\Conventional\Rifle\Model"), "rifle.omsm");
                ExportStaticModel(Asset("M11", @"ReachContent\Props\Weapons\Conventional\SMG\Model"), "smg.omsm");
                ExportStaticModel(Asset(@"Weapons\M294\Model", @"ReachContent\Props\Weapons\Conventional\LMG\Model"), "lmg.omsm");
                // The runtime resolves these through the active graphics-profile prefix.
                // This bridge creates a Reach device, so use the ReachContent variants.
                ExportStaticModel(Asset(@"ReachContent\Weapons\Rpg", @"ReachContent\Props\Weapons\Conventional\RPG\Model"), "rpg.omsm");
                ExportStaticModel(Asset(@"ReachContent\Weapons\RPGGrenade", @"ReachContent\Props\Weapons\Conventional\RPG\RPGGrenade"), "rocket.omsm");
                ExportStaticModel(Asset("Grenade", @"ReachContent\Props\Weapons\Conventional\Grenade\Model"), "grenade.omsm");
                ExportStaticModel(Asset("Spade", @"ReachContent\Props\Tools\Spade\Model"), "spade.omsm");
                ExportStaticModel(Asset("Axe", @"ReachContent\Props\Tools\Axe\Model"), "axe.omsm");
                ExportStaticModel(Asset(@"ReachContent\SpaceWeapons\AK", @"ReachContent\Props\Weapons\Space\AR\Model"), "space_ak.omsm");
                ExportStaticModel(Asset(@"ReachContent\SpaceWeapons\Colt", @"ReachContent\Props\Weapons\Space\Pistol\Model"), "space_pistol.omsm");
                ExportStaticModel(Asset(@"ReachContent\SpaceWeapons\PumpShotgun", @"ReachContent\Props\Weapons\Space\Shotgun\Model"), "space_shotgun.omsm");
                ExportStaticModel(Asset(@"ReachContent\SpaceWeapons\BoltRifle", @"ReachContent\Props\Weapons\Space\Rifle\Model"), "space_rifle.omsm");
                ExportStaticModel(Asset(@"ReachContent\SpaceWeapons\M11", @"ReachContent\Props\Weapons\Space\SMG\Model"), "space_smg.omsm");
                ExportStaticModel(Asset(@"ReachContent\Saber", @"ReachContent\Props\Weapons\Space\Saber\Model"), "saber.omsm");
                ExportStaticModel(Asset("Locator", @"ReachContent\Props\Tools\Locator\Model"), "locator.omsm");
                ExportStaticModel(Asset("Teleporter", @"ReachContent\Props\Tools\Teleporter\Model"), "teleporter.omsm");
                ExportStaticModel(Asset("Clock", @"ReachContent\Props\Tools\Clock\Model"), "clock.omsm");
                ExportStaticModel(Asset("GunPowder", @"ReachContent\Props\Items\GunPowder\Model"), "gunpowder.omsm");

                // Enemy skinned models with their embedded skeleton + animation clips,
                // plus the shared skin textures EnemyType swaps per tier.
                ExportSkinnedModel(Asset(@"Enemies\Zombies\zombieHigh", @"Enemies\Zombies\Zombie"), "enemy_zombie.omsk");
                ExportSkinnedModel(@"Enemies\Skeletons\SkeletonZombie", "enemy_skeleton_zombie.omsk");
                ExportSkinnedModel(@"Enemies\Skeletons\SkeletonArcher", "enemy_skeleton_archer.omsk");
                ExportSkinnedModel(@"Enemies\Skeletons\SkeletonAxes", "enemy_skeleton_axes.omsk");
                ExportSkinnedModel(@"Enemies\Skeletons\SkeletonSword", "enemy_skeleton_sword.omsk");
                ExportSkinnedModel(@"Enemies\Demon\Demon", "enemy_felguard.omsk");
                ExportSkinnedModel(@"Enemies\Alien\alien", "enemy_alien.omsk");
                string[] enemyTextures = new string[]
                {
                    Asset(@"Enemies\Zombies\zombie-01_0", @"Enemies\Zombies\Diffuse01_0"),
                    Asset(@"Enemies\Zombies\zombie-06", @"Enemies\Zombies\Diffuse06"),
                    Asset(@"Enemies\Zombies\zombie-10", @"Enemies\Zombies\Diffuse10"),
                    Asset(@"Enemies\Zombies\zombie-11", @"Enemies\Zombies\Diffuse11"),
                    Asset(@"Enemies\Zombies\zombie-13", @"Enemies\Zombies\Diffuse13"),
                    Asset(@"Enemies\Zombies\zombie-14", @"Enemies\Zombies\Diffuse14"),
                    Asset(@"Enemies\Zombies\zombie-17", @"Enemies\Zombies\Diffuse17"),
                    Asset(@"Enemies\Skeletons\skele-01_0", @"Enemies\Skeletons\Diffuse01_0"),
                    Asset(@"Enemies\Skeletons\skele-04", @"Enemies\Skeletons\Diffuse04"),
                    Asset(@"Enemies\Skeletons\skele-05", @"Enemies\Skeletons\Diffuse05"),
                    Asset(@"Enemies\Skeletons\skele-06", @"Enemies\Skeletons\Diffuse06"),
                    Asset(@"Enemies\Skeletons\skele-07", @"Enemies\Skeletons\Diffuse07"),
                    Asset(@"Enemies\Demon\demon_0", @"Enemies\Demon\Diffuse_0"),
                    Asset(@"Enemies\Alien\warrior_0", @"Enemies\Alien\Diffuse_0"),
                };
                for (int textureIndex = 0; textureIndex < enemyTextures.Length; ++textureIndex)
                {
                    Texture2D skin = Content.Load<Texture2D>(enemyTextures[textureIndex]);
                    Save(skin, $"enemy_texture_{textureIndex}.bmp");
                }
                try
                {
                    ExportSkinnedModel(@"Enemies\Dragon\DragonBodyHigh", "dragon_body.omsk");
                }
                catch (Exception)
                {
                    ExportStaticModel(@"Enemies\Dragon\DragonBodyHigh", "dragon_body.omsm");
                }
                string[] dragonTextures = new string[]
                {
                    @"Enemies\Dragon\dragon-01_0", @"Enemies\Dragon\dragon-09",
                    @"Enemies\Dragon\dragon-25", @"Enemies\Dragon\dragon-27",
                    @"Enemies\Dragon\dragon-35",
                };
                for (int textureIndex = 0; textureIndex < dragonTextures.Length; ++textureIndex)
                {
                    Texture2D skin = Content.Load<Texture2D>(dragonTextures[textureIndex]);
                    Save(skin, $"dragon_texture_{textureIndex}.bmp");
                }
                ExportStaticModel(Asset("FireBall", @"Props\Projectiles\FireBall\FireBall"),
                                  "fireball.omsm");
            }
            catch (Exception error)
            {
                failure = error;
            }
            finally
            {
                Exit();
            }
        }

        private void Save(Texture2D texture, string fileName)
        {
            Pixel[] pixels = TextureDecoder.Read(texture);
            BitmapWriter.Write(Path.Combine(outputDirectory, fileName), pixels, texture.Width,
                               texture.Height);
        }

        private string Asset(string legacy, string refreshed) =>
            refreshedLayout ? refreshed : legacy;

        private void SaveSprite(object manager, string spriteName, string fileName)
        {
            PropertyInfo indexer = manager.GetType().GetProperty("Item")
                ?? throw new MissingMemberException(manager.GetType().FullName, "Item");
            object sprite = indexer.GetValue(manager, new object[] { spriteName })
                ?? throw new InvalidDataException($"UI sprite {spriteName} was not found");
            Texture2D texture = (Texture2D)(sprite.GetType().GetProperty("Texture")
                ?.GetValue(sprite, null)
                ?? throw new MissingMemberException(sprite.GetType().FullName, "Texture"));
            Rectangle source = (Rectangle)(sprite.GetType().GetProperty("SourceRectangle")
                ?.GetValue(sprite, null)
                ?? throw new MissingMemberException(sprite.GetType().FullName,
                                                    "SourceRectangle"));
            Pixel[] sheet = TextureDecoder.Read(texture);
            Pixel[] crop = new Pixel[source.Width * source.Height];
            for (int y = 0; y < source.Height; ++y)
                Array.Copy(sheet, source.X + (source.Y + y) * texture.Width,
                           crop, y * source.Width, source.Width);
            BitmapWriter.Write(Path.Combine(outputDirectory, fileName), crop,
                               source.Width, source.Height);
        }

        private static VertexElement FindElement(VertexDeclaration declaration,
                                                  VertexElementUsage usage)
        {
            foreach (VertexElement element in declaration.GetVertexElements())
                if (element.VertexElementUsage == usage && element.UsageIndex == 0)
                    return element;
            throw new InvalidDataException($"Static model has no {usage} vertex element");
        }

        private static VertexElement? FindOptionalElement(VertexDeclaration declaration,
                                                          VertexElementUsage usage)
        {
            foreach (VertexElement element in declaration.GetVertexElements())
                if (element.VertexElementUsage == usage && element.UsageIndex == 0)
                    return element;
            return null;
        }

        private static float ReadSingle(byte[] bytes, int offset) =>
            BitConverter.ToSingle(bytes, offset);

        private static Vector3 ReadVector3(byte[] bytes, int baseOffset, VertexElement element)
        {
            if (element.VertexElementFormat != VertexElementFormat.Vector3)
                throw new NotSupportedException(
                    $"Unsupported {element.VertexElementUsage} format: {element.VertexElementFormat}");
            int offset = baseOffset + element.Offset;
            return new Vector3(ReadSingle(bytes, offset), ReadSingle(bytes, offset + 4),
                               ReadSingle(bytes, offset + 8));
        }

        private static Vector2 ReadVector2(byte[] bytes, int baseOffset, VertexElement element)
        {
            if (element.VertexElementFormat != VertexElementFormat.Vector2)
                throw new NotSupportedException(
                    $"Unsupported {element.VertexElementUsage} format: {element.VertexElementFormat}");
            int offset = baseOffset + element.Offset;
            return new Vector2(ReadSingle(bytes, offset), ReadSingle(bytes, offset + 4));
        }

        private static Texture2D? FirstTexture(Effect effect)
        {
            Texture2D? fallback = null;
            foreach (EffectParameter parameter in effect.Parameters)
            {
                if (parameter.ParameterType != EffectParameterType.Texture2D &&
                    parameter.ParameterType != EffectParameterType.Texture)
                    continue;
                try
                {
                    Texture2D texture = parameter.GetValueTexture2D();
                    if (texture == null)
                        continue;
                    string name = parameter.Name.ToLowerInvariant();
                    if (!name.Contains("normal") && !name.Contains("bump") &&
                        !name.Contains("spec") && !name.Contains("metal") &&
                        !name.Contains("light"))
                        return texture;
                    fallback ??= texture;
                }
                catch (InvalidOperationException)
                {
                }
            }
            return fallback;
        }

        private static Vector3 EffectDiffuse(Effect effect)
        {
            if (effect is BasicEffect basic)
                return basic.DiffuseColor;
            EffectParameter parameter = effect.Parameters["DiffuseColor"];
            if (parameter != null)
            {
                try { return parameter.GetValueVector3(); }
                catch (InvalidOperationException) { }
            }
            return Vector3.One;
        }

        private static void WriteMatrix(BinaryWriter writer, Matrix matrix)
        {
            writer.Write(matrix.M11); writer.Write(matrix.M12); writer.Write(matrix.M13); writer.Write(matrix.M14);
            writer.Write(matrix.M21); writer.Write(matrix.M22); writer.Write(matrix.M23); writer.Write(matrix.M24);
            writer.Write(matrix.M31); writer.Write(matrix.M32); writer.Write(matrix.M33); writer.Write(matrix.M34);
            writer.Write(matrix.M41); writer.Write(matrix.M42); writer.Write(matrix.M43); writer.Write(matrix.M44);
        }

        private void ExportStaticModel(string assetName, string fileName)
        {
            Model model = Content.Load<Model>(assetName);
            var manifest = new List<string>();
            Matrix[] absolute = new Matrix[model.Bones.Count];
            model.CopyAbsoluteBoneTransformsTo(absolute);
            foreach (ModelBone bone in model.Bones)
            {
                Matrix transform = absolute[bone.Index];
                FormattableString row =
                    $"bone={bone.Name} absolute={transform.M11:R},{transform.M12:R},{transform.M13:R},{transform.M14:R},{transform.M21:R},{transform.M22:R},{transform.M23:R},{transform.M24:R},{transform.M31:R},{transform.M32:R},{transform.M33:R},{transform.M34:R},{transform.M41:R},{transform.M42:R},{transform.M43:R},{transform.M44:R}";
                manifest.Add(row.ToString(CultureInfo.InvariantCulture));
            }
            int partCount = 0;
            foreach (ModelMesh mesh in model.Meshes)
                partCount += mesh.MeshParts.Count;

            using (FileStream stream = File.Create(Path.Combine(outputDirectory, fileName)))
            using (BinaryWriter writer = new BinaryWriter(stream))
            {
                writer.Write(0x4D534D4FU); // OMSM
                writer.Write(3);
                writer.Write(partCount);
                int exportedPart = 0;
                foreach (ModelMesh mesh in model.Meshes)
                {
                    Matrix meshTransform = absolute[mesh.ParentBone.Index];
                    foreach (ModelMeshPart part in mesh.MeshParts)
                    {
                        var parameters = new List<string>();
                        foreach (EffectParameter parameter in part.Effect.Parameters)
                        {
                            if (parameter.ParameterType != EffectParameterType.Texture2D &&
                                parameter.ParameterType != EffectParameterType.Texture)
                                continue;
                            try
                            {
                                Texture2D? parameterTexture = parameter.GetValueTexture2D();
                                parameters.Add(parameterTexture == null
                                    ? $"{parameter.Name}=null"
                                    : $"{parameter.Name}={parameterTexture.Width}x{parameterTexture.Height}");
                            }
                            catch (InvalidOperationException)
                            {
                                parameters.Add($"{parameter.Name}=unreadable");
                            }
                        }
                        manifest.Add($"mesh={mesh.Name} effect={part.Effect.GetType().FullName} " +
                                     string.Join(" ", parameters));
                        WriteMatrix(writer, meshTransform);
                        byte materialFlags = 0;
                        if (mesh.Name.Contains("recolor_") || mesh.Name.Contains("COLOR1"))
                            materialFlags |= 1;
                        if (mesh.Name.Contains("recolor2_") || mesh.Name.Contains("COLOR2"))
                            materialFlags |= 2;
                        writer.Write(materialFlags);
                        Vector3 diffuse = EffectDiffuse(part.Effect);
                        writer.Write(diffuse.X); writer.Write(diffuse.Y); writer.Write(diffuse.Z);

                        Texture2D? texture = FirstTexture(part.Effect);
                        if (texture == null)
                        {
                            writer.Write(0);
                            writer.Write(0);
                        }
                        else
                        {
                            Pixel[] pixels = TextureDecoder.Read(texture);
                            BitmapWriter.Write(
                                Path.Combine(outputDirectory,
                                    $"{Path.GetFileNameWithoutExtension(fileName)}_part_{exportedPart}.bmp"),
                                pixels, texture.Width, texture.Height);
                            writer.Write(texture.Width);
                            writer.Write(texture.Height);
                            foreach (Pixel pixel in pixels)
                            {
                                writer.Write(pixel.Red); writer.Write(pixel.Green);
                                writer.Write(pixel.Blue); writer.Write(pixel.Alpha);
                            }
                        }

                        VertexDeclaration declaration = part.VertexBuffer.VertexDeclaration;
                        int stride = declaration.VertexStride;
                        byte[] vertexBytes = new byte[part.VertexBuffer.VertexCount * stride];
                        part.VertexBuffer.GetData(vertexBytes);
                        VertexElement positionElement = FindElement(declaration, VertexElementUsage.Position);
                        VertexElement normalElement = FindElement(declaration, VertexElementUsage.Normal);
                        VertexElement textureElement = FindElement(declaration, VertexElementUsage.TextureCoordinate);
                        VertexElement? colorElement =
                            FindOptionalElement(declaration, VertexElementUsage.Color);
                        writer.Write(part.NumVertices);
                        for (int vertex = 0; vertex < part.NumVertices; ++vertex)
                        {
                            int offset = (part.VertexOffset + vertex) * stride;
                            Vector3 position = ReadVector3(vertexBytes, offset, positionElement);
                            Vector3 normal = ReadVector3(vertexBytes, offset, normalElement);
                            Vector2 uv = ReadVector2(vertexBytes, offset, textureElement);
                            writer.Write(position.X); writer.Write(position.Y); writer.Write(position.Z);
                            writer.Write(normal.X); writer.Write(normal.Y); writer.Write(normal.Z);
                            writer.Write(uv.X); writer.Write(uv.Y);
                            if (colorElement.HasValue)
                            {
                                if (colorElement.Value.VertexElementFormat != VertexElementFormat.Color)
                                    throw new NotSupportedException(
                                        $"Unsupported color format: {colorElement.Value.VertexElementFormat}");
                                int colorOffset = offset + colorElement.Value.Offset;
                                writer.Write(vertexBytes[colorOffset]);
                                writer.Write(vertexBytes[colorOffset + 1]);
                                writer.Write(vertexBytes[colorOffset + 2]);
                                writer.Write(vertexBytes[colorOffset + 3]);
                            }
                            else
                            {
                                writer.Write((byte)255); writer.Write((byte)255);
                                writer.Write((byte)255); writer.Write((byte)255);
                            }
                        }

                        int indexCount = part.PrimitiveCount * 3;
                        writer.Write(indexCount);
                        if (part.IndexBuffer.IndexElementSize == IndexElementSize.SixteenBits)
                        {
                            ushort[] indices = new ushort[part.IndexBuffer.IndexCount];
                            part.IndexBuffer.GetData(indices);
                            for (int index = 0; index < indexCount; ++index)
                                writer.Write((uint)indices[part.StartIndex + index]);
                        }
                        else
                        {
                            uint[] indices = new uint[part.IndexBuffer.IndexCount];
                            part.IndexBuffer.GetData(indices);
                            for (int index = 0; index < indexCount; ++index)
                                writer.Write(indices[part.StartIndex + index]);
                        }
                        ++exportedPart;
                    }
                }
            }
            File.WriteAllLines(Path.Combine(outputDirectory, fileName + ".txt"), manifest);
        }

        private object LoadUntyped(string assetName, Type assetType)
        {
            MethodInfo load = typeof(Microsoft.Xna.Framework.Content.ContentManager)
                .GetMethod("Load", BindingFlags.Instance | BindingFlags.Public)
                ?? throw new MissingMethodException("ContentManager.Load<T>");
            return load.MakeGenericMethod(assetType).Invoke(Content, new object[] { assetName })
                ?? throw new InvalidDataException($"Could not load {assetName}");
        }

        private void ExportAllAnimations()
        {
            if (refreshedLayout)
            {
                ExportRefreshedAnimations();
                return;
            }
            string animationDirectory = Path.Combine(outputDirectory, "animations");
            Directory.CreateDirectory(animationDirectory);
            var manifest = new List<string>();
            var exported = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string[] sourceDirectories = {
                Path.Combine(Content.RootDirectory, "AvatarAnimation"),
                Path.Combine(Content.RootDirectory, "SpaceWeapons", "Animations"),
            };
            string[] assetRoots = { "AvatarAnimation", @"SpaceWeapons\Animations" };
            for (int sourceIndex = 0; sourceIndex < sourceDirectories.Length; ++sourceIndex)
            {
                if (!Directory.Exists(sourceDirectories[sourceIndex]))
                    continue;
                string[] files = Directory.GetFiles(sourceDirectories[sourceIndex], "*.xnb");
                Array.Sort(files, StringComparer.OrdinalIgnoreCase);
                foreach (string sourcePath in files)
                {
                    string clipName = Path.GetFileNameWithoutExtension(sourcePath);
                    if (!exported.Add(clipName))
                        continue;
                    string assetName = Path.Combine(assetRoots[sourceIndex], clipName);
                    string relativeFile = Path.Combine("animations", clipName + ".omza");
                    ExportAnimation(assetName, relativeFile);
                    manifest.Add($"{clipName}|{assetName}|{relativeFile}");
                }
            }
            File.WriteAllLines(Path.Combine(animationDirectory, "manifest.txt"), manifest);

            // Keep the original bridge names for older native builds and external tools.
            ExportAnimation(@"AvatarAnimation\FPSIdle", "fps_idle.omza");
            ExportAnimation(@"AvatarAnimation\FPSPick", "fps_pick.omza");
            ExportAnimation(@"AvatarAnimation\GenericIdle", "generic_idle.omza");
            ExportAnimation(@"AvatarAnimation\GenericUse", "generic_use.omza");
            ExportAnimation(@"AvatarAnimation\BlockHoldIdle", "block_idle.omza");
            ExportAnimation(@"AvatarAnimation\BlockUse", "block_use.omza");
            ExportAnimation(@"AvatarAnimation\HoldPistolIdle", "pistol_idle.omza");
            ExportAnimation(@"AvatarAnimation\PistolShoot", "pistol_use.omza");
        }

        private void ExportRefreshedAnimations()
        {
            string animationDirectory = Path.Combine(outputDirectory, "animations");
            Directory.CreateDirectory(animationDirectory);
            var manifest = new List<string>();
            Action<string, string, string?> export = (assetName, clipName, rootFile) =>
            {
                string relative = Path.Combine("animations", clipName + ".omza");
                ExportAnimation(assetName, relative);
                manifest.Add($"{clipName}|{assetName}|{relative}");
                if (rootFile != null)
                    File.Copy(Path.Combine(outputDirectory, relative),
                              Path.Combine(outputDirectory, rootFile), true);
            };

            export(@"Character\Animation\Stand0", "Stand0", null);
            // Player.UpdateAnimation always keeps a full-body locomotion clip on
            // channel zero.  GenericWalk and the weapon/item clips below are
            // upper-body overlays; exporting only those leaves network avatars'
            // legs in the bind pose even though PlayerUpdate carries movement.
            export(@"Character\Animation\Walk", "Walk", null);
            export(@"Character\Animation\Run", "Run", null);
            export(@"Character\Animation\Faint", "Faint", null);
            export(@"Character\Animation\Swim Underwater", "Swim", null);
            export(@"Character\Animation\GenericIdle", "GenericIdle", "generic_idle.omza");
            export(@"Character\Animation\GenericUse", "GenericUse", "generic_use.omza");
            export(@"Character\Animation\GenericWalk", "GenericWalk", null);
            export(@"Props\Tools\PickAxe\Animation\Idle", "FPSIdle", "fps_idle.omza");
            export(@"Props\Tools\PickAxe\Animation\Use", "FPSPick", "fps_pick.omza");
            export(@"Props\Tools\PickAxe\Animation\Walk", "PickWalk", null);
            export(@"Props\Items\Block\Animation\Idle", "BlockHoldIdle", "block_idle.omza");
            export(@"Props\Items\Block\Animation\Use", "BlockUse", "block_use.omza");
            export(@"Props\Items\Block\Animation\Walk", "BlockWalk", null);

            string ar = @"Props\Weapons\Conventional\AR\Animation\";
            export(ar + "Idle", "HoldAssultIdle", null);
            export(ar + "Walk", "AKRun", null);
            export(ar + "Shoot", "AssaultShoot", null);
            export(ar + "Shoulder", "AssaultShoulder", null);
            export(ar + "ShoulderIdle", "AssaultShoulderIdle", null);
            export(ar + "ShoulderWalk", "AssaultShoulderWalk", null);
            export(ar + "ShoulderShoot", "AssaultShoulderShoot", null);
            export(ar + "Reload", "AssaultReload", null);

            string pistol = @"Props\Weapons\Conventional\Pistol\Animation\";
            export(pistol + "Idle", "HoldPistolIdle", "pistol_idle.omza");
            export(pistol + "Walk", "PistolWalk", null);
            export(pistol + "Shoot", "PistolShoot", "pistol_use.omza");
            export(pistol + "Shoulder", "PistolShoulder", null);
            export(pistol + "ShoulderIdle", "PistolShoulderIdle", null);
            export(pistol + "ShoulderWalk", "PistolShoulderWalk", null);
            export(pistol + "ShoulderShoot", "PistolShoulderShoot", null);
            export(pistol + "Reload", "PistolReload", null);

            string shotgun = @"Props\Weapons\Conventional\Shotgun\Animation\";
            export(shotgun + "Shoot", "PumpShotgunShoot", null);
            export(shotgun + "ShoulderShoot", "PumpShotgunShoulderShoot", null);
            export(shotgun + "Reload", "PumpShotgunReload", null);

            string rifle = @"Props\Weapons\Conventional\Rifle\Animation\";
            export(rifle + "Idle", "RifleIdle", null);
            export(rifle + "Walk", "RifleWalk", null);
            export(rifle + "Shoot", "RifleShoot", null);
            export(rifle + "Shoulder", "RifleShoulder", null);
            export(rifle + "ShoulderIdle", "RifleShoulderIdle", null);
            export(rifle + "ShoulderWalk", "RifleShoulderWalk", null);
            export(rifle + "ShoulderShoot", "RifleShoulderShoot", null);
            export(rifle + "Reload", "RifleReload", null);

            string smg = @"Props\Weapons\Conventional\SMG\Animation\";
            export(smg + "Idle", "SMGIdle", null);
            export(smg + "Walk", "SMGWalk", null);
            export(smg + "Shoot", "SMGShoot", null);
            export(smg + "Shoulder", "SMGShoulder", null);
            export(smg + "ShoulderIdle", "SMGShoulderIdle", null);
            export(smg + "ShoulderWalk", "SMGShoulderWalk", null);
            export(smg + "ShoulderShoot", "SMGShoulderShoot", null);
            export(smg + "Reload", "SMGReload", null);

            // Space weapons have distinct PlayerMode values and authored body
            // clips. They are not reskins of the conventional families: the
            // stock game registers these under the Laser* animation names.
            string laserAr = @"Props\Weapons\Space\AR\Animation\";
            export(laserAr + "Idle", "LaserGunIdle", null);
            export(laserAr + "Walk", "LaserGunRun", null);
            export(laserAr + "Shoot", "LaserGunShoot", null);
            export(laserAr + "Shoulder", "LaserGunShoulder", null);
            export(laserAr + "ShoulderIdle", "LaserGunShoulderIdle", null);
            export(laserAr + "ShoulderWalk", "LaserGunShoulderWalk", null);
            export(laserAr + "ShoulderShoot", "LaserGunShoulderShoot", null);
            export(laserAr + "Reload", "LaserGunReload", null);

            string laserPistol = @"Props\Weapons\Space\Pistol\Animation\";
            export(laserPistol + "Idle", "LaserPistolIdle", null);
            export(laserPistol + "Walk", "LaserPistolRun", null);
            export(laserPistol + "Shoot", "LaserPistolShoot", null);
            export(laserPistol + "Shoulder", "LaserPistolShoulder", null);
            export(laserPistol + "ShoulderIdle", "LaserPistolShoulderIdle", null);
            export(laserPistol + "ShoulderWalk", "LaserPistolShoulderWalk", null);
            export(laserPistol + "ShoulderShoot", "LaserPistolShoulderShoot", null);
            export(laserPistol + "Reload", "LaserPistolReload", null);

            string laserShotgun = @"Props\Weapons\Space\Shotgun\Animation\";
            export(laserShotgun + "Shoot", "LaserShotgunShoot", null);
            export(laserShotgun + "ShoulderShoot", "LaserShotgunShoulderShoot", null);
            export(laserShotgun + "Reload", "LaserShotgunReload", null);

            string laserRifle = @"Props\Weapons\Space\Rifle\Animation\";
            export(laserRifle + "Idle", "LaserRifleIdle", null);
            export(laserRifle + "Walk", "LaserRifleRun", null);
            export(laserRifle + "Shoot", "LaserRifleShoot", null);
            export(laserRifle + "Shoulder", "LaserRifleShoulder", null);
            export(laserRifle + "ShoulderIdle", "LaserRifleShoulderIdle", null);
            export(laserRifle + "ShoulderWalk", "LaserRifleShoulderWalk", null);
            export(laserRifle + "ShoulderShoot", "LaserRifleShoulderShoot", null);
            export(laserRifle + "Reload", "LaserRifleReload", null);

            string laserSmg = @"Props\Weapons\Space\SMG\Animation\";
            export(laserSmg + "Idle", "LaserSMGIdle", null);
            export(laserSmg + "Walk", "LaserSMGRun", null);
            export(laserSmg + "Shoot", "LaserSMGShoot", null);
            export(laserSmg + "Shoulder", "LaserSMGShoulder", null);
            export(laserSmg + "ShoulderIdle", "LaserSMGShoulderIdle", null);
            export(laserSmg + "ShoulderWalk", "LaserSMGShoulderWalk", null);
            export(laserSmg + "ShoulderShoot", "LaserSMGShoulderShoot", null);
            export(laserSmg + "Reload", "LaserSMGReload", null);

            string lmg = @"Props\Weapons\Conventional\LMG\Animation\";
            export(lmg + "Idle", "LMGIdle", null);
            export(lmg + "Walk", "LMGRun", null);
            export(lmg + "Shoot", "LMGShoot", null);
            export(lmg + "Shoulder", "LMGShoulder", null);
            export(lmg + "ShoulderIdle", "LMGShoulderIdle", null);
            export(lmg + "ShoulderWalk", "LMGShoulderWalk", null);
            export(lmg + "ShoulderShoot", "LMGShoulderShoot", null);
            export(lmg + "Reload", "LMGReload", null);

            string rpg = @"Props\Weapons\Conventional\RPG\Animation\";
            export(rpg + "Idle", "RPGHold", null);
            export(rpg + "Walk", "RPGWalk", null);
            export(rpg + "Shoot", "RPGShoot", null);

            string grenade = @"Props\Weapons\Conventional\Grenade\Animation\";
            export(grenade + "Idle", "GrenadeIdle", null);
            export(grenade + "Cook", "GrenadeCook", null);
            export(grenade + "Throw", "GrenadeThrow", null);
            export(grenade + "Release", "GrenadeRelease", null);
            File.WriteAllLines(Path.Combine(animationDirectory, "manifest.txt"), manifest);
        }

        private void ExportSkinnedModel(string assetName, string fileName)
        {
            Model model = Content.Load<Model>(assetName);
            object skinning = model.Tag
                ?? throw new InvalidDataException($"{assetName} carries no skinning data");
            Type skinningType = skinning.GetType();
            object skeleton = skinningType.GetProperty("Skeleton")!.GetValue(skinning, null)!;
            var inverseBind = (Matrix[])skinningType.GetProperty("InverseBindPose")!
                .GetValue(skinning, null)!;
            Type animationDataType = skinningType.BaseType!;
            var clips = (System.Collections.IDictionary)animationDataType
                .GetProperty("AnimationClips")!.GetValue(skinning, null)!;

            var bones = ((System.Collections.IEnumerable)skeleton).Cast<object>().ToList();
            Type boneType = bones[0].GetType();
            PropertyInfo boneName = boneType.GetProperty("Name")!;
            PropertyInfo boneParent = boneType.GetProperty("Parent")!;
            PropertyInfo boneIndex = boneType.GetProperty("Index")!;
            PropertyInfo boneTransform = boneType.GetProperty("Transform")!;

            using FileStream stream = File.Create(Path.Combine(outputDirectory, fileName));
            using BinaryWriter writer = new BinaryWriter(stream);
            writer.Write(0x4B534D4FU); // OMSK
            writer.Write(1);
            writer.Write(bones.Count);
            foreach (object bone in bones)
            {
                writer.Write((string)boneName.GetValue(bone, null)!);
                object? parent = boneParent.GetValue(bone, null);
                writer.Write(parent == null ? -1 : (int)boneIndex.GetValue(parent, null)!);
                WriteMatrix(writer, (Matrix)boneTransform.GetValue(bone, null)!);
            }
            writer.Write(inverseBind.Length);
            foreach (Matrix matrix in inverseBind)
                WriteMatrix(writer, matrix);

            int partCount = 0;
            foreach (ModelMesh mesh in model.Meshes)
                partCount += mesh.MeshParts.Count;
            writer.Write(partCount);
            foreach (ModelMesh mesh in model.Meshes)
            {
                foreach (ModelMeshPart part in mesh.MeshParts)
                {
                    Texture2D? texture = FirstTexture(part.Effect);
                    if (texture == null)
                    {
                        writer.Write(0);
                        writer.Write(0);
                    }
                    else
                    {
                        Pixel[] pixels = TextureDecoder.Read(texture);
                        writer.Write(texture.Width);
                        writer.Write(texture.Height);
                        foreach (Pixel pixel in pixels)
                        {
                            writer.Write(pixel.Red); writer.Write(pixel.Green);
                            writer.Write(pixel.Blue); writer.Write(pixel.Alpha);
                        }
                    }
                    VertexDeclaration declaration = part.VertexBuffer.VertexDeclaration;
                    int stride = declaration.VertexStride;
                    byte[] vertexBytes = new byte[part.VertexBuffer.VertexCount * stride];
                    part.VertexBuffer.GetData(vertexBytes);
                    VertexElement positionElement = FindElement(declaration, VertexElementUsage.Position);
                    VertexElement normalElement = FindElement(declaration, VertexElementUsage.Normal);
                    VertexElement textureElement = FindElement(declaration, VertexElementUsage.TextureCoordinate);
                    VertexElement? blendIndexElement =
                        FindOptionalElement(declaration, VertexElementUsage.BlendIndices);
                    VertexElement? blendWeightElement =
                        FindOptionalElement(declaration, VertexElementUsage.BlendWeight);
                    writer.Write(part.NumVertices);
                    for (int vertex = 0; vertex < part.NumVertices; ++vertex)
                    {
                        int offset = (part.VertexOffset + vertex) * stride;
                        Vector3 position = ReadVector3(vertexBytes, offset, positionElement);
                        Vector3 normal = ReadVector3(vertexBytes, offset, normalElement);
                        Vector2 uv = ReadVector2(vertexBytes, offset, textureElement);
                        writer.Write(position.X); writer.Write(position.Y); writer.Write(position.Z);
                        writer.Write(normal.X); writer.Write(normal.Y); writer.Write(normal.Z);
                        writer.Write(uv.X); writer.Write(uv.Y);
                        if (blendIndexElement.HasValue)
                        {
                            int blendOffset = offset + blendIndexElement.Value.Offset;
                            writer.Write(vertexBytes[blendOffset]);
                            writer.Write(vertexBytes[blendOffset + 1]);
                            writer.Write(vertexBytes[blendOffset + 2]);
                            writer.Write(vertexBytes[blendOffset + 3]);
                        }
                        else
                        {
                            writer.Write((byte)0); writer.Write((byte)0);
                            writer.Write((byte)0); writer.Write((byte)0);
                        }
                        if (blendWeightElement.HasValue)
                        {
                            int weightOffset = offset + blendWeightElement.Value.Offset;
                            writer.Write(ReadSingle(vertexBytes, weightOffset));
                            writer.Write(ReadSingle(vertexBytes, weightOffset + 4));
                            writer.Write(ReadSingle(vertexBytes, weightOffset + 8));
                            writer.Write(ReadSingle(vertexBytes, weightOffset + 12));
                        }
                        else
                        {
                            writer.Write(1f); writer.Write(0f); writer.Write(0f); writer.Write(0f);
                        }
                    }
                    int indexCount = part.PrimitiveCount * 3;
                    writer.Write(indexCount);
                    if (part.IndexBuffer.IndexElementSize == IndexElementSize.SixteenBits)
                    {
                        ushort[] indices = new ushort[part.IndexBuffer.IndexCount];
                        part.IndexBuffer.GetData(indices);
                        for (int index = 0; index < indexCount; ++index)
                            writer.Write((uint)indices[part.StartIndex + index]);
                    }
                    else
                    {
                        uint[] indices = new uint[part.IndexBuffer.IndexCount];
                        part.IndexBuffer.GetData(indices);
                        for (int index = 0; index < indexCount; ++index)
                            writer.Write(indices[part.StartIndex + index]);
                    }
                }
            }

            Type clipType = Type.GetType("DNA.Drawing.Animation.AnimationClip, DNA.Common", true);
            PropertyInfo clipDuration = clipType.GetProperty("Duration")!;
            PropertyInfo clipBoneCount = clipType.GetProperty("BoneCount")!;
            MethodInfo copyTransforms = clipType.GetMethod("CopyTransforms")!;
            var sidecar = new List<string>();
            foreach (object bone in bones)
                sidecar.Add($"bone {(int)boneIndex.GetValue(bone, null)!} " +
                            $"{(string)boneName.GetValue(bone, null)!}");
            foreach (System.Collections.DictionaryEntry entry in clips)
                sidecar.Add($"clip {(string)entry.Key} " +
                            $"{((TimeSpan)clipDuration.GetValue(entry.Value, null)!).TotalSeconds:F4}");
            File.WriteAllLines(Path.Combine(outputDirectory, fileName + ".txt"), sidecar);
            writer.Write(clips.Count);
            const int sampleRate = 30;
            foreach (System.Collections.DictionaryEntry entry in clips)
            {
                string clipName = (string)entry.Key;
                object clip = entry.Value!;
                int boneCount = (int)clipBoneCount.GetValue(clip, null)!;
                TimeSpan duration = (TimeSpan)clipDuration.GetValue(clip, null)!;
                int frameCount = Math.Max(2,
                    (int)Math.Ceiling(duration.TotalSeconds * sampleRate) + 1);
                writer.Write(clipName);
                writer.Write(boneCount);
                writer.Write(frameCount);
                writer.Write((float)duration.TotalSeconds);
                writer.Write(sampleRate);
                Vector3[] translations = new Vector3[boneCount];
                Quaternion[] rotations = new Quaternion[boneCount];
                Vector3[] scales = new Vector3[boneCount];
                bool[] influence = new bool[boneCount];
                for (int index = 0; index < boneCount; ++index)
                    influence[index] = true;
                for (int frame = 0; frame < frameCount; ++frame)
                {
                    TimeSpan position = TimeSpan.FromSeconds(
                        Math.Min(duration.TotalSeconds, frame / (double)sampleRate));
                    copyTransforms.Invoke(clip, new object[] {
                        translations, rotations, scales, position, influence });
                    for (int bone = 0; bone < boneCount; ++bone)
                    {
                        writer.Write(translations[bone].X);
                        writer.Write(translations[bone].Y);
                        writer.Write(translations[bone].Z);
                        writer.Write(rotations[bone].X);
                        writer.Write(rotations[bone].Y);
                        writer.Write(rotations[bone].Z);
                        writer.Write(rotations[bone].W);
                        writer.Write(scales[bone].X);
                        writer.Write(scales[bone].Y);
                        writer.Write(scales[bone].Z);
                    }
                }
            }
        }

        private void ExportAnimation(string assetName, string fileName)
        {
            Type clipType = Type.GetType("DNA.Drawing.Animation.AnimationClip, DNA.Common", true);
            object clip = LoadUntyped(assetName, clipType);
            int boneCount = (int)clipType.GetProperty("BoneCount").GetValue(clip, null);
            TimeSpan duration = (TimeSpan)clipType.GetProperty("Duration").GetValue(clip, null);
            MethodInfo copy = clipType.GetMethod("CopyTransforms")
                ?? throw new MissingMethodException("AnimationClip.CopyTransforms");

            // AvatarRenderer.parentBoneIDs from Microsoft.Xna.Framework.Avatar 4.0.
            // This stable 71-bone Xbox avatar hierarchy is the skeleton used by
            // both FPS clips and by every .omavatar export.
            int[] parents = {
                -1, 0, 0, 0, 0, 1, 2, 2, 3, 3,
                1, 6, 5, 6, 5, 8, 5, 8, 5, 14,
                12, 11, 16, 15, 14, 20, 20, 20, 22, 22,
                22, 25, 25, 25, 28, 28, 28, 33, 33, 33,
                33, 33, 33, 33, 36, 36, 36, 36, 36, 36,
                36, 37, 38, 39, 40, 43, 44, 45, 46, 47,
                50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
                60
            };
            if (parents.Length != boneCount)
                throw new InvalidDataException("Unexpected Xbox avatar bone count");

            const int sampleRate = 30;
            int frameCount = Math.Max(2, (int)Math.Ceiling(duration.TotalSeconds * sampleRate) + 1);
            Vector3[] translations = new Vector3[boneCount];
            Quaternion[] rotations = new Quaternion[boneCount];
            Vector3[] scales = new Vector3[boneCount];
            bool[] influence = new bool[boneCount];
            for (int index = 0; index < boneCount; ++index)
                influence[index] = true;

            using (FileStream stream = File.Create(Path.Combine(outputDirectory, fileName)))
            using (BinaryWriter writer = new BinaryWriter(stream))
            {
                writer.Write(0x415A4D4FU); // OMZA
                writer.Write(1);
                writer.Write(boneCount);
                writer.Write(frameCount);
                writer.Write((float)duration.TotalSeconds);
                writer.Write(sampleRate);
                foreach (int parent in parents)
                    writer.Write(parent);
                for (int frame = 0; frame < frameCount; ++frame)
                {
                    TimeSpan position = TimeSpan.FromSeconds(
                        Math.Min(duration.TotalSeconds, frame / (double)sampleRate));
                    copy.Invoke(clip, new object[] {
                        translations, rotations, scales, position, influence
                    });
                    for (int bone = 0; bone < boneCount; ++bone)
                    {
                        writer.Write(translations[bone].X);
                        writer.Write(translations[bone].Y);
                        writer.Write(translations[bone].Z);
                        writer.Write(rotations[bone].X);
                        writer.Write(rotations[bone].Y);
                        writer.Write(rotations[bone].Z);
                        writer.Write(rotations[bone].W);
                        writer.Write(scales[bone].X);
                        writer.Write(scales[bone].Y);
                        writer.Write(scales[bone].Z);
                    }
                }
            }
        }

        private void ExportParticle(string assetName, string fileStem)
        {
            Type effectType = Type.GetType("DNA.Drawing.Particles.ParticleEffect, DNA.Common", true);
            object effect = LoadUntyped(assetName, effectType);
            PropertyInfo textureProperty = effectType.GetProperty("Texture");
            if (textureProperty?.GetValue(effect, null) is Texture2D texture)
                Save(texture, fileStem + ".bmp");

            string[] propertyNames = {
                "TexturePath", "NumTilesWide", "NumTilesHigh", "FirstTileToInclude",
                "LastTileToInclude", "BlendMode", "Technique", "RandomizeRotations",
                "ParticlesPerSecond", "LocalSpace", "FadeOut", "EmmissionTime",
                "ParticleLifeTime", "LifetimeVariation", "EmitterVelocitySensitivity",
                "HorizontalVelocityMin", "HorizontalVelocityMax", "VerticalVelocityMin",
                "VerticalVelocityMax", "Gravity", "VelocityEnd", "ColorMin", "ColorMax",
                "RotateSpeedMin", "RotateSpeedMax", "StartSizeMin", "StartSizeMax",
                "EndSizeMin", "EndSizeMax", "DieAfterEmmision"
            };
            using (StreamWriter writer = File.CreateText(
                Path.Combine(outputDirectory, fileStem + ".txt")))
            {
                foreach (string propertyName in propertyNames)
                {
                    PropertyInfo property = effectType.GetProperty(propertyName);
                    object? value = property?.GetValue(effect, null);
                    writer.WriteLine($"{propertyName}={value}");
                }
            }
        }

    }

    internal static class Program
    {
        private static int Main(string[] args)
        {
            try
            {
                string gameDirectory = args.Length > 0
                    ? Path.GetFullPath(args[0])
                    : @"C:\Program Files (x86)\Steam\steamapps\common\CastleMiner Z";
                string contentDirectory = Path.Combine(gameDirectory, "Content");
                AppDomain.CurrentDomain.AssemblyResolve += (_, request) =>
                {
                    string assemblyPath = Path.Combine(
                        gameDirectory,
                        new AssemblyName(request.Name).Name + ".dll");
                    return File.Exists(assemblyPath) ? Assembly.LoadFrom(assemblyPath) : null;
                };
                if (!File.Exists(Path.Combine(contentDirectory, "Terrain", "Textures.xnb")) &&
                    !File.Exists(Path.Combine(contentDirectory, "ReachContent", "Terrain",
                                              "Textures.xnb")))
                {
                    Console.Error.WriteLine($"CastleMiner Z content was not found under {gameDirectory}");
                    return 2;
                }
                string outputDirectory = args.Length > 1
                    ? Path.GetFullPath(args[1])
                    : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                   "OpenMinerZ", "ContentPacks",
                                   File.Exists(Path.Combine(contentDirectory, "ReachContent",
                                                            "Terrain", "Textures.xnb"))
                                       ? "modern" : "classic",
                                   "DerivedData");
                using (var game = new BridgeGame(contentDirectory, outputDirectory))
                {
                    game.Run();
                    game.ThrowIfFailed();
                }
                string[] sourceFiles = Directory.GetFiles(
                    contentDirectory, "*", SearchOption.AllDirectories);
                long sourceBytes = 0;
                foreach (string sourceFile in sourceFiles)
                    sourceBytes += new FileInfo(sourceFile).Length;
                File.WriteAllLines(Path.Combine(outputDirectory, "pack-manifest.txt"),
                    new string[]
                    {
                        "format=OpenMinerZ content pack 1",
                        "style=" + (File.Exists(Path.Combine(contentDirectory, "ReachContent",
                                                               "Terrain", "Textures.xnb"))
                                       ? "modern" : "classic"),
                        "source_files=" + sourceFiles.Length.ToString(),
                        "source_bytes=" + sourceBytes.ToString(),
                    });
                Console.WriteLine($"Derived original content: {outputDirectory}");
                Console.WriteLine("The build target stages the complete source Content tree beside this derived pack.");
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine(error);
                return 1;
            }
        }
    }
}
