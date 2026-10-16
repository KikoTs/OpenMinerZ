using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Text;
using HarmonyLib;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;

// Everything the shipped screens submit to XNA, in submission order.
//
// SpriteBatch's public Draw overloads all funnel into the private InternalDraw
// and every DrawString into SpriteFont.InternalDraw, so two hooks see every
// quad and every string no matter which overload the game used. A third hook on
// DNA's DrawOutlinedText folds its five DrawString calls back into the one
// outlined string the game asked for.
//
// Record grammar (one per line, keys in this fixed order):
//   S name=<sprite> [sub=x,y,w,h] dst=x,y,w,h rgba=r,g,b,a [rot=..] [fx=..]
//   T font=<font> pos=x,y scale=sx,sy rgba=r,g,b,a [outline=w orgba=r,g,b,a] text="..."
//   E <event>                      sound cue, pickup, crate message, screen change
// `name` is the sprite's key in the shipped sheet, or one of
//   @white          the 1x1 white texture (flat fills)
//   @item:<id>      a 64x64 cell of the item-icon atlas InventoryItem renders
//   @offscreen      the 1280x720 buffer Classic composes into (the letterbox blit)
//   @cursor         the game's own mouse pointer
internal static class Recorder
{
    public static bool Enabled;
    public static readonly List<string> Lines = new List<string>();
    public static readonly List<string> Events = new List<string>();

    // texture -> (source rectangle -> sprite name)
    private static readonly Dictionary<Texture2D, List<KeyValuePair<Rectangle, string>>> sheets =
        new Dictionary<Texture2D, List<KeyValuePair<Rectangle, string>>>();
    private static readonly Dictionary<Texture2D, string> textures = new Dictionary<Texture2D, string>();
    private static readonly Dictionary<SpriteFont, string> fonts = new Dictionary<SpriteFont, string>();
    private static readonly HashSet<Texture2D> fontTextures = new HashSet<Texture2D>();
    public static Func<Texture2D> ItemAtlas = () => null;
    public static Func<Texture2D> ItemAtlasLarge = () => null;
    public static Func<Texture2D> Offscreen = () => null;

    private static int textDepth;
    private static int outlineDepth;
    private static FieldInfo proxyString, proxyBuilder;

    public static void RegisterSheet(object spriteManager)
    {
        var sprites = (IDictionary)R.Get(spriteManager, "_sprites");
        foreach (DictionaryEntry entry in sprites)
        {
            var texture = (Texture2D)R.Get(entry.Value, "Texture");
            var source = (Rectangle)R.Get(entry.Value, "SourceRectangle");
            List<KeyValuePair<Rectangle, string>> list;
            if (!sheets.TryGetValue(texture, out list)) sheets[texture] = list = new List<KeyValuePair<Rectangle, string>>();
            list.Add(new KeyValuePair<Rectangle, string>(source, (string)entry.Key));
        }
    }

    public static void RegisterTexture(Texture2D texture, string name) { if (texture != null) textures[texture] = name; }

    public static void RegisterFont(SpriteFont font, string name)
    {
        if (font == null) return;
        fonts[font] = name;
        fontTextures.Add((Texture2D)R.Get(font, "textureValue"));
    }

    public static string F(float value)
    {
        return Math.Round(value, 3).ToString("0.###", CultureInfo.InvariantCulture);
    }

    private static string C(Color color) { return color.R + "," + color.G + "," + color.B + "," + color.A; }

    public static string Quote(string text)
    {
        var builder = new StringBuilder("\"");
        foreach (char c in text)
        {
            if (c == '"' || c == '\\') builder.Append('\\').Append(c);
            else if (c == '\n') builder.Append("\\n");
            else if (c == '\r') { }
            else builder.Append(c);
        }
        return builder.Append('"').ToString();
    }

    public static void Event(string text)
    {
        Events.Add(text);
        if (Enabled) Lines.Add("E " + text);
    }

    private static string SpriteName(Texture2D texture, Rectangle? source, out string sub)
    {
        sub = null;
        string name;
        if (textures.TryGetValue(texture, out name)) return name;
        Texture2D large = ItemAtlasLarge();
        if (large != null && ReferenceEquals(texture, large))
        {
            // 1.9.9.8's 128x128 atlas, chosen when an icon is drawn wider than 70 px.
            Rectangle cell = source ?? new Rectangle(0, 0, texture.Width, texture.Height);
            if (cell.Width == 128 && cell.Height == 128 && cell.X % 128 == 0 && cell.Y % 128 == 0)
                return "@item-large:" + (cell.Y / 128 * 8 + cell.X / 128);
        }
        if (ReferenceEquals(texture, ItemAtlas()))
        {
            Rectangle cell = source ?? new Rectangle(0, 0, texture.Width, texture.Height);
            if (cell.Width == 64 && cell.Height == 64 && cell.X % 64 == 0 && cell.Y % 64 == 0)
                return "@item:" + (cell.Y / 64 * 8 + cell.X / 64);
            sub = cell.X + "," + cell.Y + "," + cell.Width + "," + cell.Height;
            return "@items";
        }
        if (ReferenceEquals(texture, Offscreen())) return "@offscreen";
        List<KeyValuePair<Rectangle, string>> list;
        if (sheets.TryGetValue(texture, out list))
        {
            Rectangle cell = source ?? new Rectangle(0, 0, texture.Width, texture.Height);
            foreach (var pair in list) if (pair.Key == cell) return pair.Value;
            // A part of a sprite (Sprite.Draw with an inner source rectangle).
            foreach (var pair in list)
                if (pair.Key.Contains(cell))
                {
                    sub = (cell.X - pair.Key.X) + "," + (cell.Y - pair.Key.Y) + "," + cell.Width + "," + cell.Height;
                    return pair.Value;
                }
            sub = cell.X + "," + cell.Y + "," + cell.Width + "," + cell.Height;
            return "@sheet";
        }
        if (source.HasValue)
            sub = source.Value.X + "," + source.Value.Y + "," + source.Value.Width + "," + source.Value.Height;
        return "@texture" + texture.Width + "x" + texture.Height;
    }

    // SpriteBatch.InternalDraw(texture, ref destination, scaleDestination, ref source,
    //                          color, rotation, ref origin, effects, depth)
    public static void SpritePrefix(object[] __args)
    {
        if (!Enabled || textDepth != 0 || Suppressed != 0) return;
        var texture = (Texture2D)__args[0];
        if (fontTextures.Contains(texture)) return;
        var destination = (Vector4)__args[1];
        bool scale = (bool)__args[2];
        var source = (Rectangle?)__args[3];
        var color = (Color)__args[4];
        float rotation = (float)__args[5];
        var origin = (Vector2)__args[6];
        var effects = (SpriteEffects)__args[7];
        float sourceWidth = source.HasValue ? source.Value.Width : texture.Width;
        float sourceHeight = source.HasValue ? source.Value.Height : texture.Height;
        float width = scale ? destination.Z * sourceWidth : destination.Z;
        float height = scale ? destination.W * sourceHeight : destination.W;
        // XNA scales the origin with the sprite; report the top-left corner.
        float left = destination.X - origin.X * (sourceWidth == 0 ? 0 : width / sourceWidth);
        float top = destination.Y - origin.Y * (sourceHeight == 0 ? 0 : height / sourceHeight);
        string sub;
        string name = SpriteName(texture, source, out sub);
        var line = new StringBuilder("S name=" + name);
        if (sub != null) line.Append(" sub=" + sub);
        line.Append(" dst=" + F(left) + "," + F(top) + "," + F(width) + "," + F(height));
        line.Append(" rgba=" + C(color));
        if (rotation != 0f) line.Append(" rot=" + F(rotation));
        if (effects != SpriteEffects.None) line.Append(" fx=" + effects);
        Lines.Add(line.ToString());
    }

    // SpriteFont.InternalDraw(ref text, spriteBatch, position, color, rotation,
    //                         origin, ref scale, effects, depth)
    public static void TextPrefix(SpriteFont __instance, object[] __args)
    {
        ++textDepth;
        if (!Enabled || outlineDepth != 0 || Suppressed != 0) return;
        object proxy = __args[0];
        if (proxyString == null)
        {
            proxyString = proxy.GetType().GetField("textString", BindingFlags.Instance | BindingFlags.NonPublic);
            proxyBuilder = proxy.GetType().GetField("textBuilder", BindingFlags.Instance | BindingFlags.NonPublic);
        }
        string text = (string)proxyString.GetValue(proxy) ?? proxyBuilder.GetValue(proxy).ToString();
        var position = (Vector2)__args[2];
        var color = (Color)__args[3];
        var origin = (Vector2)__args[5];
        var scale = (Vector2)__args[6];
        Lines.Add(TextLine(__instance, text, position - origin * scale, scale, color, 0, default(Color)));
    }

    public static void TextPostfix() { --textDepth; }

    private static string TextLine(SpriteFont font, string text, Vector2 position, Vector2 scale, Color color,
                                   int outline, Color outlineColor)
    {
        string name;
        if (!fonts.TryGetValue(font, out name)) name = "@font" + font.LineSpacing;
        var line = new StringBuilder("T font=" + name);
        line.Append(" pos=" + F(position.X) + "," + F(position.Y));
        line.Append(" scale=" + F(scale.X) + "," + F(scale.Y));
        line.Append(" rgba=" + C(color));
        if (outline != 0) line.Append(" outline=" + outline + " orgba=" + C(outlineColor));
        line.Append(" text=" + Quote(text));
        return line.ToString();
    }

    // DrawingTools.DrawOutlinedText(spriteBatch, font, text|builder, Point|Vector2 location,
    //                               textColor, outlineColor, outlineWidth[, scale, rotation, origin])
    public static void OutlinePrefix(object[] __args)
    {
        ++outlineDepth;
        if (!Enabled || outlineDepth != 1 || Suppressed != 0) return;
        var font = (SpriteFont)__args[1];
        string text = __args[2].ToString();
        Vector2 position = __args[3] is Point ? new Vector2(((Point)__args[3]).X, ((Point)__args[3]).Y) : (Vector2)__args[3];
        var color = (Color)__args[4];
        var outlineColor = (Color)__args[5];
        int width = (int)__args[6];
        float scale = __args.Length > 7 ? (float)__args[7] : 1f;
        Vector2 origin = __args.Length > 9 ? (Vector2)__args[9] : Vector2.Zero;
        Lines.Add(TextLine(font, text, position - origin * scale, new Vector2(scale, scale), color, width, outlineColor));
    }

    public static void OutlinePostfix() { --outlineDepth; }

    // Every SpriteFont.MeasureString the screens make while laying out, so the
    // native tests can answer the same questions without the fonts.
    public static readonly SortedSet<string> Measures = new SortedSet<string>(StringComparer.Ordinal);

    public static void MeasurePostfix(SpriteFont __instance, object[] __args, Vector2 __result)
    {
        string name;
        if (!fonts.TryGetValue(__instance, out name)) return;
        object proxy = __args[0];
        if (proxyString == null)
        {
            proxyString = proxy.GetType().GetField("textString", BindingFlags.Instance | BindingFlags.NonPublic);
            proxyBuilder = proxy.GetType().GetField("textBuilder", BindingFlags.Instance | BindingFlags.NonPublic);
        }
        string text = (string)proxyString.GetValue(proxy) ?? proxyBuilder.GetValue(proxy).ToString();
        Measures.Add(name + " " + F(__result.X) + " " + F(__result.Y) + " " + Quote(text));
    }

    // Draws that belong to another screen (the HUD's stat panels inside
    // 1.9.9.8's inventory) are announced by an E record and not itemised.
    public static int Suppressed;
    public static void SuppressPrefix(MethodBase __originalMethod)
    {
        if (Enabled && Suppressed == 0) Lines.Add("E hud " + __originalMethod.Name);
        ++Suppressed;
    }
    public static void SuppressPostfix() { --Suppressed; }

    public static bool Skip() { return false; }

    public static bool SoundPrefix(object[] __args)
    {
        Event("sound " + __args[0]);
        return false;
    }

    public static void Install(Harmony harmony, Assembly common)
    {
        var prefix = new HarmonyMethod(typeof(Recorder).GetMethod("SpritePrefix"));
        harmony.Patch(AccessTools.Method(typeof(SpriteBatch), "InternalDraw"), prefix);
        harmony.Patch(AccessTools.Method(typeof(SpriteFont), "InternalMeasure"), null,
                      new HarmonyMethod(typeof(Recorder).GetMethod("MeasurePostfix")));
        harmony.Patch(AccessTools.Method(typeof(SpriteFont), "InternalDraw"),
                      new HarmonyMethod(typeof(Recorder).GetMethod("TextPrefix")),
                      new HarmonyMethod(typeof(Recorder).GetMethod("TextPostfix")));
        Type tools = common.GetType("DNA.Drawing.DrawingTools", true);
        foreach (MethodInfo method in tools.GetMethods(BindingFlags.Public | BindingFlags.Static)
                     .Where(candidate => candidate.Name == "DrawOutlinedText"))
            harmony.Patch(method, new HarmonyMethod(typeof(Recorder).GetMethod("OutlinePrefix")),
                          new HarmonyMethod(typeof(Recorder).GetMethod("OutlinePostfix")));
    }
}
