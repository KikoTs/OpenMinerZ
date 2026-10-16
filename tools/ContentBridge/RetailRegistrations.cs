#nullable disable
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

// Shared by ContentBridge (import) and tools/OriginalClientInterop/RigParityProbe.
//
// The retail game registers every character clip inside
// CastleMinerZGame.SecondaryLoad, in the middle of start-up code that cannot run
// outside the game (screens, sound banks, terrain). The registrations are
// straight-line statements with constant operands:
//
//     AvatarAnimationManager.Instance.RegisterAnimation(
//         "GunIdle", Content.Load<AnimationClip>("AvatarAnimation\\HoldAssultIdle"),
//         looping: true, new AvatarBone[1] { AvatarBone.BackUpper });
//
// Rather than transcribing ~110 of those per generation, this walks the method's
// IL in the user's own executable and recovers each call's constants. Nothing is
// executed and no game text is stored in the repository.
namespace OpenMinerZ.RetailCharacter
{
    public sealed class ClipRegistration
    {
        public string Name;          // the name Player.UpdateAnimation plays, e.g. "GunIdle"
        public string Asset;         // content path of the male clip
        public string FemaleAsset;   // content path of the female clip, or null
        public bool Looping;
        public int[] Bones;          // influence roots (AvatarBone ordinals); null = every bone
        public int[] MaskedBones;    // roots removed from the influence set; null = none
    }

    public static class RegistrationScanner
    {
        private sealed class ClipOperand { public string Asset; }

        private static readonly OpCode[] OneByte = new OpCode[256];
        private static readonly OpCode[] TwoByte = new OpCode[256];

        static RegistrationScanner()
        {
            foreach (FieldInfo field in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                OpCode code = (OpCode)field.GetValue(null);
                ushort value = unchecked((ushort)code.Value);
                if (value < 0x100) OneByte[value] = code;
                else if ((value & 0xFF00) == 0xFE00) TwoByte[value & 0xFF] = code;
            }
        }

        public static List<ClipRegistration> Extract(Assembly game)
        {
            Type gameType = game.GetType("DNA.CastleMinerZ.CastleMinerZGame", true);
            const BindingFlags all = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public |
                                     BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
            var result = new List<ClipRegistration>();
            foreach (MethodInfo method in gameType.GetMethods(all))
            {
                MethodBody body = method.GetMethodBody();
                if (body != null)
                    Scan(method, body.GetILAsByteArray(), result);
            }
            return result;
        }

        private static object Pop(List<object> stack)
        {
            if (stack.Count == 0) return null;
            object value = stack[stack.Count - 1];
            stack.RemoveAt(stack.Count - 1);
            return value;
        }

        private static void Scan(MethodBase method, byte[] il, List<ClipRegistration> output)
        {
            Module module = method.Module;
            Type[] typeArguments = method.DeclaringType.IsGenericType ? method.DeclaringType.GetGenericArguments() : null;
            Type[] methodArguments = method.IsGenericMethod ? method.GetGenericArguments() : null;
            var stack = new List<object>();
            var locals = new Dictionary<int, object>();
            int offset = 0;
            while (offset < il.Length)
            {
                OpCode code;
                byte first = il[offset++];
                if (first == 0xFE) code = TwoByte[il[offset++]];
                else code = OneByte[first];
                int operand = offset;
                switch (code.OperandType)
                {
                    case OperandType.InlineNone: break;
                    case OperandType.ShortInlineBrTarget:
                    case OperandType.ShortInlineI:
                    case OperandType.ShortInlineVar: offset += 1; break;
                    case OperandType.InlineVar: offset += 2; break;
                    case OperandType.InlineI8:
                    case OperandType.InlineR: offset += 8; break;
                    case OperandType.InlineSwitch: offset += 4 + 4 * BitConverter.ToInt32(il, offset); break;
                    default: offset += 4; break;
                }

                if (code.FlowControl == FlowControl.Branch || code.FlowControl == FlowControl.Cond_Branch ||
                    code.FlowControl == FlowControl.Return || code.FlowControl == FlowControl.Throw)
                {
                    // Registrations are straight-line statements; anything spanning a
                    // jump is not one, so the model stack simply starts again.
                    stack.Clear();
                    continue;
                }

                short value = code.Value;
                if (code == OpCodes.Ldstr) { stack.Add(module.ResolveString(BitConverter.ToInt32(il, operand))); continue; }
                if (code == OpCodes.Ldc_I4) { stack.Add(BitConverter.ToInt32(il, operand)); continue; }
                if (code == OpCodes.Ldc_I4_S) { stack.Add((int)(sbyte)il[operand]); continue; }
                if (code == OpCodes.Ldc_I4_M1) { stack.Add(-1); continue; }
                if (value >= OpCodes.Ldc_I4_0.Value && value <= OpCodes.Ldc_I4_8.Value)
                {
                    stack.Add(value - OpCodes.Ldc_I4_0.Value);
                    continue;
                }
                if (code == OpCodes.Ldnull) { stack.Add(null); continue; }
                // The shipped compilers build `new AvatarBone[1] { X }` through a
                // temporary local rather than `dup`, so locals are modelled too.
                int local = -1;
                bool store = false;
                if (value >= OpCodes.Stloc_0.Value && value <= OpCodes.Stloc_3.Value) { local = value - OpCodes.Stloc_0.Value; store = true; }
                else if (value >= OpCodes.Ldloc_0.Value && value <= OpCodes.Ldloc_3.Value) local = value - OpCodes.Ldloc_0.Value;
                else if (code == OpCodes.Stloc_S) { local = il[operand]; store = true; }
                else if (code == OpCodes.Ldloc_S) local = il[operand];
                else if (code == OpCodes.Stloc) { local = BitConverter.ToUInt16(il, operand); store = true; }
                else if (code == OpCodes.Ldloc) local = BitConverter.ToUInt16(il, operand);
                if (local >= 0)
                {
                    if (store) locals[local] = Pop(stack);
                    else { object held; locals.TryGetValue(local, out held); stack.Add(held); }
                    continue;
                }
                if (code == OpCodes.Dup)
                {
                    stack.Add(stack.Count > 0 ? stack[stack.Count - 1] : null);
                    continue;
                }
                if (code == OpCodes.Newarr)
                {
                    object length = Pop(stack);
                    stack.Add(length is int ? new int[(int)length] : null);
                    continue;
                }
                if (code == OpCodes.Stelem_I4 || code == OpCodes.Stelem || code == OpCodes.Stelem_I1 ||
                    code == OpCodes.Stelem_I2 || code == OpCodes.Stelem_Ref || code == OpCodes.Stelem_I ||
                    code == OpCodes.Stelem_I8 || code == OpCodes.Stelem_R4 || code == OpCodes.Stelem_R8)
                {
                    object element = Pop(stack), index = Pop(stack), array = Pop(stack);
                    int[] values = array as int[];
                    if (values != null && index is int && element is int && (int)index >= 0 && (int)index < values.Length)
                        values[(int)index] = (int)element;
                    continue;
                }
                if (code == OpCodes.Ldtoken)
                {
                    object token = null;
                    try { token = module.ResolveField(BitConverter.ToInt32(il, operand), typeArguments, methodArguments); }
                    catch (Exception) { }
                    stack.Add(token);
                    continue;
                }
                if (code == OpCodes.Call || code == OpCodes.Callvirt || code == OpCodes.Newobj)
                {
                    MethodBase target;
                    try { target = module.ResolveMethod(BitConverter.ToInt32(il, operand), typeArguments, methodArguments); }
                    catch (Exception) { stack.Clear(); continue; }
                    ParameterInfo[] parameters = target.GetParameters();
                    object[] arguments = new object[parameters.Length];
                    for (int index = parameters.Length - 1; index >= 0; --index) arguments[index] = Pop(stack);
                    if (!target.IsStatic && code != OpCodes.Newobj) Pop(stack);

                    object produced = null;
                    if (target.Name == "InitializeArray" && target.DeclaringType == typeof(RuntimeHelpers))
                    {
                        // Larger constant arrays are stored as a data blob and copied in.
                        if (arguments[0] is int[] && arguments[1] is FieldInfo)
                            RuntimeHelpers.InitializeArray((Array)arguments[0], ((FieldInfo)arguments[1]).FieldHandle);
                        continue;
                    }
                    if (target.Name == "Load" && target.IsGenericMethod && arguments.Length == 1 &&
                        target.GetGenericArguments()[0].Name == "AnimationClip")
                    {
                        produced = new ClipOperand { Asset = arguments[0] as string };
                    }
                    else if (target.Name == "RegisterAnimation" && target.DeclaringType.Name == "AvatarAnimationManager")
                    {
                        var registration = new ClipRegistration { Name = arguments[0] as string };
                        int next = 1;
                        ClipOperand male = arguments[next++] as ClipOperand;
                        registration.Asset = male == null ? null : male.Asset;
                        if (parameters[next].ParameterType.Name == "AnimationClip")
                        {
                            ClipOperand female = arguments[next++] as ClipOperand;
                            registration.FemaleAsset = female == null ? null : female.Asset;
                        }
                        registration.Looping = arguments[next] is int && (int)arguments[next] != 0;
                        ++next;
                        if (next < arguments.Length) registration.Bones = arguments[next++] as int[];
                        if (next < arguments.Length) registration.MaskedBones = arguments[next++] as int[];
                        if (registration.Name == null || registration.Asset == null)
                            throw new InvalidOperationException(
                                "A RegisterAnimation call in " + method.Name + " did not have constant operands");
                        output.Add(registration);
                    }
                    MethodInfo info = target as MethodInfo;
                    if (code == OpCodes.Newobj || (info != null && info.ReturnType != typeof(void)))
                        stack.Add(produced);
                    continue;
                }

                // Everything else only needs its stack arithmetic to stay in step.
                int pops = PopCount(code.StackBehaviourPop);
                int pushes = PushCount(code.StackBehaviourPush);
                if (pops < 0 || pushes < 0) { stack.Clear(); continue; }
                for (int index = 0; index < pops; ++index) Pop(stack);
                for (int index = 0; index < pushes; ++index) stack.Add(null);
            }
        }

        private static int PopCount(StackBehaviour behaviour)
        {
            switch (behaviour)
            {
                case StackBehaviour.Pop0: return 0;
                case StackBehaviour.Pop1:
                case StackBehaviour.Popi:
                case StackBehaviour.Popref: return 1;
                case StackBehaviour.Pop1_pop1:
                case StackBehaviour.Popi_pop1:
                case StackBehaviour.Popi_popi:
                case StackBehaviour.Popi_popi8:
                case StackBehaviour.Popi_popr4:
                case StackBehaviour.Popi_popr8:
                case StackBehaviour.Popref_pop1:
                case StackBehaviour.Popref_popi: return 2;
                case StackBehaviour.Popi_popi_popi:
                case StackBehaviour.Popref_popi_popi:
                case StackBehaviour.Popref_popi_popi8:
                case StackBehaviour.Popref_popi_popr4:
                case StackBehaviour.Popref_popi_popr8:
                case StackBehaviour.Popref_popi_popref:
                case StackBehaviour.Popref_popi_pop1: return 3;
                default: return -1;
            }
        }

        private static int PushCount(StackBehaviour behaviour)
        {
            switch (behaviour)
            {
                case StackBehaviour.Push0: return 0;
                case StackBehaviour.Push1:
                case StackBehaviour.Pushi:
                case StackBehaviour.Pushi8:
                case StackBehaviour.Pushr4:
                case StackBehaviour.Pushr8:
                case StackBehaviour.Pushref: return 1;
                case StackBehaviour.Push1_push1: return 2;
                default: return -1;
            }
        }
    }
}
