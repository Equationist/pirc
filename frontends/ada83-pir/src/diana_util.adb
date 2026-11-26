-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Characters.Handling;
with Ada.Long_Float_Text_IO;
with Ada.Strings.Fixed;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Unchecked_Conversion;

package body Diana_Util is

   type UDigit_Pair is record
      D1 : SHORT;
      D2 : SHORT;
   end record;
   pragma Pack (UDigit_Pair);
   for UDigit_Pair'Size use 32;

   function To_UDigit_Pair is new Ada.Unchecked_Conversion (TREE, UDigit_Pair);
   function To_U16 is new Ada.Unchecked_Conversion (SHORT, Unsigned_16);
   function To_F64 is new Ada.Unchecked_Conversion (Unsigned_64, IEEE_Float_64);
   function To_U64 is new Ada.Unchecked_Conversion (IEEE_Float_64, Unsigned_64);

   Max_Big_Limbs : constant Natural := 512;
   type Big_Limb_Arr is array (1 .. Max_Big_Limbs) of Natural;

   -- Unified base-10000 DIANA DN_NUM_VAL limb decoder (replaces 4 duplicated decoders)
   procedure Decode_Num_Val_Limbs
     (Node   : TREE;
      Limbs  : out Big_Limb_Arr;
      Len    : out Natural;
      Is_Neg : out Boolean;
      Trunc  : out Boolean)
   is
   begin
      Limbs  := (others => 0);
      Len    := 1;
      Is_Neg := False;
      Trunc  := False;
      if Is_Void_Tree (Node) or else KIND (Node) /= DN_NUM_VAL then
         return;
      end if;
      if Node.TLN = 0 then
         declare
            V : Integer := Integer (Node.TPG);
         begin
            if V < 0 then
               Is_Neg := True;
               V := -V;
            end if;
            Limbs (1) := V mod 10000;
            if V >= 10000 then
               Limbs (2) := V / 10000;
               Len := 2;
            end if;
         end;
         return;
      end if;
      declare
         Hdr  : constant TREE := DABS (0, Node);
         Wlen : constant Integer := Integer (Hdr.TLN);
         ND   : Integer := 2 * Wlen;
      begin
         if Wlen <= 0 then
            return;
         end if;
         if ND > Max_Big_Limbs then
            Trunc := True;
            ND := Max_Big_Limbs;
         end if;
         for J in 1 .. Wlen loop
            declare
               Pair : constant UDigit_Pair := To_UDigit_Pair (DABS (J, Node));
               D1   : constant Integer := Integer (Pair.D1);
               D2   : constant Integer := Integer (Pair.D2);
            begin
               if J * 2 - 1 <= Max_Big_Limbs then
                  Limbs (J * 2 - 1) := (if D1 >= 0 then D1 else 0);
               elsif D1 /= 0 then
                  Trunc := True;
               end if;
               if J * 2 <= Max_Big_Limbs then
                  Limbs (J * 2) := (if D2 >= 0 then D2 else 0);
               elsif D2 /= 0 then
                  Trunc := True;
               end if;
            end;
         end loop;
         if Limbs (1) >= 10000 then
            Limbs (1) := Limbs (1) - 10000;
            Is_Neg := True;
         end if;
         while ND > 1 and then Limbs (ND) = 0 loop
            ND := ND - 1;
         end loop;
         Len := Natural (ND);
      end;
   end Decode_Num_Val_Limbs;

   function Cmp_Big (A : Big_Limb_Arr; AL : Natural; B : Big_Limb_Arr; BL : Natural) return Integer is
   begin
      if AL < BL then return -1; end if;
      if AL > BL then return 1; end if;
      for I in reverse 1 .. AL loop
         if A (I) < B (I) then return -1; end if;
         if A (I) > B (I) then return 1; end if;
      end loop;
      return 0;
   end Cmp_Big;

   procedure Mul2_Big (A : in out Big_Limb_Arr; AL : in out Natural) is
      Carry : Natural := 0;
   begin
      for I in 1 .. AL loop
         declare
            T : constant Natural := A (I) * 2 + Carry;
         begin
            A (I) := T mod 10000;
            Carry := T / 10000;
         end;
      end loop;
      if Carry > 0 and then AL < Max_Big_Limbs then
         AL := AL + 1;
         A (AL) := Carry;
      end if;
   end Mul2_Big;

   procedure Sub_Big (A : in out Big_Limb_Arr; AL : in out Natural; B : Big_Limb_Arr; BL : Natural) is
      Borrow : Integer := 0;
   begin
      for I in 1 .. AL loop
         declare
            BV : constant Integer := (if I <= BL then Integer (B (I)) else 0);
            D  : Integer := Integer (A (I)) - BV - Borrow;
         begin
            if D < 0 then
               D := D + 10000;
               Borrow := 1;
            else
               Borrow := 0;
            end if;
            A (I) := Natural (D);
         end;
      end loop;
      while AL > 1 and then A (AL) = 0 loop
         AL := AL - 1;
      end loop;
   end Sub_Big;

   function Eval_Rational_To_F64
     (N_Init : Big_Limb_Arr;
      N_Len0 : Natural;
      D_Init : Big_Limb_Arr;
      D_Len0 : Natural;
      Neg    : Boolean) return IEEE_Float_64
   is
      N_Digs     : Big_Limb_Arr := N_Init;
      D_Digs     : Big_Limb_Arr := D_Init;
      N_Len      : Natural := N_Len0;
      D_Len      : Natural := D_Len0;
      Exp        : Integer := 0;
      Bits       : Integer;
      Biased_Exp : Integer;
      Q          : Unsigned_64 := 0;
      Mant_Bits  : Unsigned_64 := 0;
      C          : Integer;
      Bits64     : Unsigned_64;
   begin
      if (N_Len = 1 and then N_Digs (1) = 0) or else (D_Len = 1 and then D_Digs (1) = 0) then
         return 0.0;
      end if;
      -- Normalize so D <= N < 2*D, with true value = (N / D) * 2^Exp
      if Cmp_Big (N_Digs, N_Len, D_Digs, D_Len) < 0 then
         while Cmp_Big (N_Digs, N_Len, D_Digs, D_Len) < 0 and then Exp > -1100 loop
            Mul2_Big (N_Digs, N_Len);
            Exp := Exp - 1;
         end loop;
      else
         while Cmp_Big (N_Digs, N_Len, D_Digs, D_Len) >= 0 and then Exp < 1030 loop
            Mul2_Big (D_Digs, D_Len);
            Exp := Exp + 1;
         end loop;
         Mul2_Big (N_Digs, N_Len);
         Exp := Exp - 1;
      end if;
      if Exp >= 1024 then
         Bits64 := (if Neg then 16#FFF0_0000_0000_0000# else 16#7FF0_0000_0000_0000#);
         return To_F64 (Bits64);
      elsif Exp < -1075 then
         return 0.0;
      end if;
      if Exp >= -1022 then
         Bits := 53;
         Biased_Exp := Exp + 1023;
      else
         Bits := 53 - (-1022 - Exp);
         Biased_Exp := 0;
      end if;
      if Bits = 0 then
         -- Exp = -1075: value is (N / D) * 0.5 * 2^-1074 with D <= N < 2*D;
         -- exact tie (N = D) rounds to even (0), strictly greater (N > D) rounds up to 1.
         if Cmp_Big (N_Digs, N_Len, D_Digs, D_Len) > 0 then
            Q := 1;
         end if;
      else
         for B in 1 .. Bits loop
            Q := Shift_Left (Q, 1);
            if Cmp_Big (N_Digs, N_Len, D_Digs, D_Len) >= 0 then
               Sub_Big (N_Digs, N_Len, D_Digs, D_Len);
               Q := Q or 1;
            end if;
            if B < Bits then
               Mul2_Big (N_Digs, N_Len);
            end if;
         end loop;
         -- Round to nearest-even using remainder N / D
         Mul2_Big (N_Digs, N_Len);
         C := Cmp_Big (N_Digs, N_Len, D_Digs, D_Len);
         if C > 0 or else (C = 0 and then (Q and 1) = 1) then
            Q := Q + 1;
         end if;
      end if;
      if Biased_Exp > 0 then
         if Q >= Shift_Left (Unsigned_64 (1), 53) then
            Q := Shift_Right (Q, 1);
            Biased_Exp := Biased_Exp + 1;
         end if;
         if Biased_Exp >= 2047 then
            Bits64 := (if Neg then 16#FFF0_0000_0000_0000# else 16#7FF0_0000_0000_0000#);
            return To_F64 (Bits64);
         end if;
         Mant_Bits := Q and 16#000F_FFFF_FFFF_FFFF#;
      else
         if Q >= Shift_Left (Unsigned_64 (1), 52) then
            Biased_Exp := 1;
            Mant_Bits := Q and 16#000F_FFFF_FFFF_FFFF#;
         else
            Mant_Bits := Q;
         end if;
      end if;
      Bits64 := (if Neg then 16#8000_0000_0000_0000# else 0)
        or Shift_Left (Unsigned_64 (Biased_Exp), 52)
        or Mant_Bits;
      return To_F64 (Bits64);
   end Eval_Rational_To_F64;

   function Tree_Key (T : TREE) return Unsigned_32 is
      Pg : constant Unsigned_32 := Unsigned_32 (To_U16 (T.TPG));
      Ty : constant Unsigned_32 := Unsigned_32 (T.TTY);
      Ln : constant Unsigned_32 := Unsigned_32 (T.TLN);
   begin
      return Shift_Left (Pg, 16) or Shift_Left (Ty, 8) or Ln;
   end Tree_Key;

   function Tree_Key_Str (T : TREE) return String is
      K : constant String := Unsigned_32'Image (Tree_Key (T));
   begin
      if K (K'First) = ' ' then
         return K (K'First + 1 .. K'Last);
      end if;
      return K;
   end Tree_Key_Str;

   function Is_Void_Tree (T : TREE) return Boolean is
   begin
      return T = CONST_VOID or else T = CONST_NIL or else T = CONST_VIRGIN
        or else KIND (T) = DN_VOID or else KIND (T) = DN_NIL;
   end Is_Void_Tree;

   function Sym_Name (T : TREE) return String is
   begin
      if Is_Void_Tree (T) then
         return "";
      end if;
      if KIND (T) = DN_SYMBOL_REP or else KIND (T) = DN_TXTREP then
         return PRINTNAME (T);
      end if;
      if KIND (T) not in CLASS_DEF_NAME | CLASS_DESIGNATOR | DN_STRING_LITERAL then
         return "";
      end if;
      declare
         S : constant TREE := D (LX_SYMREP, T);
      begin
         if Is_Void_Tree (S) then
            return "";
         end if;
         return PRINTNAME (S);
      end;
   end Sym_Name;

   function Lower_Name (S : String) return String is
   begin
      return Ada.Characters.Handling.To_Lower (S);
   end Lower_Name;

   function Sanitize_Ident (S : String) return String is
      L : constant String := Lower_Name (S);
      R : Unbounded_String;
   begin
      if L = """+""" then return "op_plus";
      elsif L = """-""" then return "op_minus";
      elsif L = """*""" then return "op_mul";
      elsif L = """/""" then return "op_div";
      elsif L = """mod""" then return "op_mod";
      elsif L = """rem""" then return "op_rem";
      elsif L = """**""" then return "op_exp";
      elsif L = """=""" then return "op_eq";
      elsif L = """/=""" then return "op_ne";
      elsif L = """<""" then return "op_lt";
      elsif L = """<=""" then return "op_le";
      elsif L = """>""" then return "op_gt";
      elsif L = """>=""" then return "op_ge";
      elsif L = """&""" then return "op_cat";
      elsif L = """and""" then return "op_and";
      elsif L = """or""" then return "op_or";
      elsif L = """xor""" then return "op_xor";
      elsif L = """not""" then return "op_not";
      elsif L = """abs""" then return "op_abs";
      end if;
      for C of L loop
         if (C >= 'a' and then C <= 'z')
           or else (C >= '0' and then C <= '9')
           or else C = '_'
         then
            Append (R, C);
         else
            Append (R, '_');
         end if;
      end loop;
      if Length (R) = 0 then
         return "anon";
      end if;
      return To_String (R);
   end Sanitize_Ident;

   function Get_Name_Defn (Name_Node : TREE) return TREE is
      Cur : TREE := Name_Node;
   begin
      while not Is_Void_Tree (Cur) and then KIND (Cur) = DN_SELECTED loop
         Cur := D (AS_DESIGNATOR, Cur);
      end loop;
      if Is_Void_Tree (Cur) then
         return CONST_VOID;
      end if;
      if KIND (Cur) in CLASS_DESIGNATOR then
         return D (SM_DEFN, Cur);
      end if;
      return Cur;
   end Get_Name_Defn;

   function Canonical_Def (Id : TREE) return TREE is
   begin
      if Is_Void_Tree (Id) then
         return CONST_VOID;
      end if;
      case KIND (Id) is
         when DN_PROCEDURE_ID | DN_FUNCTION_ID | DN_OPERATOR_ID
            | DN_PACKAGE_ID | DN_GENERIC_ID
            | DN_IN_ID | DN_OUT_ID | DN_IN_OUT_ID
            | DN_TYPE_ID | DN_CONSTANT_ID | DN_DISCRIMINANT_ID =>
            declare
               F : constant TREE := D (SM_FIRST, Id);
            begin
               if not Is_Void_Tree (F) then
                  return F;
               end if;
            end;
         when others =>
            null;
      end case;
      return Id;
   end Canonical_Def;

   function Canonical_Exc (Exc_Id : TREE) return TREE is
      Cur : TREE := Exc_Id;
   begin
      for Step in 1 .. 16 loop
         exit when Is_Void_Tree (Cur);
         exit when KIND (Cur) /= DN_EXCEPTION_ID;
         declare
            Ren : constant TREE := D (SM_RENAMES_EXC, Cur);
         begin
            exit when Is_Void_Tree (Ren);
            if KIND (Ren) = DN_EXCEPTION_ID then
               Cur := Ren;
            elsif KIND (Ren) in CLASS_DESIGNATOR or else KIND (Ren) = DN_SELECTED then
               declare
                  Target : TREE := Ren;
               begin
                  while not Is_Void_Tree (Target) and then KIND (Target) = DN_SELECTED loop
                     Target := D (AS_DESIGNATOR, Target);
                  end loop;
                  if not Is_Void_Tree (Target) and then KIND (Target) in CLASS_DESIGNATOR then
                     Cur := D (SM_DEFN, Target);
                  else
                     exit;
                  end if;
               end;
            else
               exit;
            end if;
         end;
      end loop;
      return Cur;
   end Canonical_Exc;

   function Resolve_Type_Spec (T : TREE) return TREE is
      Cur : TREE := T;
   begin
      for Step in 1 .. 16 loop
         exit when Is_Void_Tree (Cur);
         case KIND (Cur) is
            when DN_PRIVATE | DN_L_PRIVATE =>
               declare
                  Full : constant TREE := D (SM_TYPE_SPEC, Cur);
                  Der  : constant TREE := D (SM_DERIVED, Cur);
               begin
                  if not Is_Void_Tree (Full) then
                     Cur := Full;
                  elsif not Is_Void_Tree (Der) then
                     Cur := Der;
                  else
                     exit;
                  end if;
               end;
            when DN_INCOMPLETE =>
               declare
                  XFull : constant TREE := D (XD_FULL_TYPE_SPEC, Cur);
               begin
                  if not Is_Void_Tree (XFull) then
                     Cur := XFull;
                  else
                     exit;
                  end if;
               end;
            when others =>
               exit;
         end case;
      end loop;
      return Cur;
   end Resolve_Type_Spec;

   function Base_Type_Spec (T : TREE) return TREE is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return CONST_VOID;
      end if;
      if KIND (R) in CLASS_NON_TASK then
         declare
            B : constant TREE := D (SM_BASE_TYPE, R);
         begin
            if not Is_Void_Tree (B) then
               return Resolve_Type_Spec (B);
            end if;
         end;
      end if;
      return R;
   end Base_Type_Spec;

   function Safe_SM_Value (Node : TREE) return TREE is
   begin
      if Is_Void_Tree (Node) then
         return CONST_VOID;
      end if;
      case KIND (Node) is
         when DN_USED_CHAR | DN_USED_OBJECT_ID
            | DN_ATTRIBUTE | DN_SELECTED
            | DN_NUMERIC_LITERAL | DN_NULL_ACCESS
            | DN_FUNCTION_CALL
            | DN_CONVERSION | DN_QUALIFIED | DN_PARENTHESIZED
            | DN_SHORT_CIRCUIT | DN_RANGE_MEMBERSHIP | DN_TYPE_MEMBERSHIP =>
            return D (SM_VALUE, Node);
         when others =>
            return CONST_VOID;
      end case;
   end Safe_SM_Value;

   function Has_Static_Val (T : TREE) return Boolean is
   begin
      if Is_Void_Tree (T) then
         return False;
      end if;
      return KIND (T) = DN_NUM_VAL or else KIND (T) = DN_REAL_VAL;
   end Has_Static_Val;

   procedure Limbs_To_I64_Checked
     (Digs       : Big_Limb_Arr;
      Ndig       : Natural;
      Neg        : Boolean;
      Trunc      : Boolean;
      Val        : out Integer_64;
      Overflowed : out Boolean)
   is
      Acc : Unsigned_64 := 0;
      Ovf : Boolean := Trunc;
   begin
      Overflowed := False;
      for I in reverse 1 .. Ndig loop
         declare
            D_U : constant Unsigned_64 := Unsigned_64 (Digs (I));
         begin
            if Acc > (Unsigned_64'Last - D_U) / 10000 then
               Ovf := True;
               Acc := Unsigned_64'Last;
            else
               Acc := Acc * 10000 + D_U;
            end if;
         end;
      end loop;
      if Neg then
         if Ovf or else Acc > 16#8000_0000_0000_0000# then
            Val := Integer_64'First;
            Overflowed := True;
         elsif Acc = 16#8000_0000_0000_0000# then
            Val := Integer_64'First;
         else
            Val := -Integer_64 (Acc);
         end if;
      else
         if Ovf or else Acc >= 16#8000_0000_0000_0000# then
            Val := Integer_64'Last;
            Overflowed := True;
         else
            Val := Integer_64 (Acc);
         end if;
      end if;
   end Limbs_To_I64_Checked;

   procedure Eval_Int64_Checked
     (Val_Node   : TREE;
      Val        : out Integer_64;
      Overflowed : out Boolean)
   is
   begin
      Val := 0;
      Overflowed := False;
      if Is_Void_Tree (Val_Node) then
         return;
      end if;
      if KIND (Val_Node) = DN_REAL_VAL then
         -- Exact rational evaluation when denominator is 1 or small, falling back to safe f64 check
         declare
            NNode : constant TREE := D (XD_NUMER, Val_Node);
            DNode : constant TREE := D (XD_DENOM, Val_Node);
         begin
            if not Is_Void_Tree (DNode) and then KIND (DNode) = DN_NUM_VAL
              and then DNode.TLN = 0 and then DNode.TPG = 1
            then
               Eval_Int64_Checked (NNode, Val, Overflowed);
               return;
            end if;
            declare
               F : constant IEEE_Float_64 := Eval_Float64 (Val_Node);
               R : IEEE_Float_64;
            begin
               if not (F = F) then
                  Val := 0;
                  Overflowed := True;
                  return;
               end if;
               R := IEEE_Float_64'Rounding (F);
               -- IEEE_Float_64(Integer_64'Last) = 2.0**63 = 9223372036854775808.0 which is > Integer_64'Last!
               if R >= 9223372036854775808.0 then
                  Val := Integer_64'Last;
                  Overflowed := True;
               elsif R < -9223372036854775808.0 then
                  Val := Integer_64'First;
                  Overflowed := True;
               elsif R = -9223372036854775808.0 then
                  Val := Integer_64'First;
               else
                  Val := Integer_64 (R);
               end if;
            end;
         end;
         return;
      end if;
      if KIND (Val_Node) /= DN_NUM_VAL then
         return;
      end if;
      if Val_Node.TLN = 0 then
         Val := Integer_64 (Val_Node.TPG);
         return;
      end if;
      declare
         Digs  : Big_Limb_Arr;
         Ndig  : Natural;
         Neg   : Boolean;
         Trunc : Boolean;
      begin
         Decode_Num_Val_Limbs (Val_Node, Digs, Ndig, Neg, Trunc);
         Limbs_To_I64_Checked (Digs, Ndig, Neg, Trunc, Val, Overflowed);
      end;
   end Eval_Int64_Checked;

   function Eval_Int64 (Val_Node : TREE) return Integer_64 is
      V   : Integer_64;
      Ovf : Boolean;
   begin
      Eval_Int64_Checked (Val_Node, V, Ovf);
      pragma Unreferenced (Ovf);
      return V;
   end Eval_Int64;

   function Eval_Float64 (Val_Node : TREE) return IEEE_Float_64 is
   begin
      if Is_Void_Tree (Val_Node) then
         return 0.0;
      end if;
      if KIND (Val_Node) = DN_NUM_VAL then
         declare
            N_Digs : Big_Limb_Arr;
            D_Digs : Big_Limb_Arr := (1 => 1, others => 0);
            N_Len  : Natural;
            N_Neg  : Boolean;
            Trunc  : Boolean;
         begin
            Decode_Num_Val_Limbs (Val_Node, N_Digs, N_Len, N_Neg, Trunc);
            if Trunc then
               return To_F64 (if N_Neg then 16#FFF0_0000_0000_0000# else 16#7FF0_0000_0000_0000#);
            end if;
            return Eval_Rational_To_F64 (N_Digs, N_Len, D_Digs, 1, N_Neg);
         end;
      end if;
      if KIND (Val_Node) = DN_REAL_VAL then
         declare
            NNode  : constant TREE := D (XD_NUMER, Val_Node);
            DNode  : constant TREE := D (XD_DENOM, Val_Node);
            N_Digs, D_Digs : Big_Limb_Arr;
            N_Len, D_Len   : Natural;
            N_Neg, D_Neg   : Boolean;
            N_Tr, D_Tr     : Boolean;
         begin
            Decode_Num_Val_Limbs (NNode, N_Digs, N_Len, N_Neg, N_Tr);
            Decode_Num_Val_Limbs (DNode, D_Digs, D_Len, D_Neg, D_Tr);
            pragma Unreferenced (N_Tr, D_Tr);
            return Eval_Rational_To_F64 (N_Digs, N_Len, D_Digs, D_Len, N_Neg xor D_Neg);
         end;
      end if;
      return 0.0;
   end Eval_Float64;

   function Format_Float64 (V : IEEE_Float_64) return String is
      Bits : constant Unsigned_64 := To_U64 (V);
      Exp  : constant Unsigned_64 := Shift_Right (Bits, 52) and 16#7FF#;
      Frac : constant Unsigned_64 := Bits and 16#000F_FFFF_FFFF_FFFF#;
      Neg  : constant Boolean := (Shift_Right (Bits, 63) and 1) /= 0;
   begin
      if Exp = 16#7FF# then
         if Frac /= 0 then
            return "nan";
         elsif Neg then
            return "-inf";
         else
            return "inf";
         end if;
      end if;
      -- Print full 17 significant digits (Aft => 16) for exact IEEE-754 f64 round-trip!
      declare
         Buf : String (1 .. 32);
      begin
         Ada.Long_Float_Text_IO.Put (Buf, Long_Float (V), Aft => 16, Exp => 3);
         return Ada.Strings.Fixed.Trim (Buf, Ada.Strings.Both);
      exception
         when others =>
            declare
               S : constant String := Ada.Strings.Fixed.Trim (IEEE_Float_64'Image (V), Ada.Strings.Both);
            begin
               if Ada.Strings.Fixed.Index (S, ".") = 0 and then Ada.Strings.Fixed.Index (S, "E") = 0 then
                  return S & ".0";
               end if;
               return S;
            end;
      end;
   end Format_Float64;

   function Is_Standard_Root_Enum (T : TREE; Name : String) return Boolean is
      B   : TREE := Base_Type_Spec (T);
      Src : TREE;
      Reg : TREE;
   begin
      if Is_Void_Tree (B) or else KIND (B) /= DN_ENUMERATION then
         return False;
      end if;
      for Step in 1 .. 16 loop
         exit when Is_Void_Tree (B) or else KIND (B) /= DN_ENUMERATION;
         declare
            Der : constant TREE := D (SM_DERIVED, B);
         begin
            exit when Is_Void_Tree (Der);
            B := Base_Type_Spec (Der);
         end;
      end loop;
      if Is_Void_Tree (B) or else KIND (B) /= DN_ENUMERATION then
         return False;
      end if;
      Src := D (XD_SOURCE_NAME, B);
      if Is_Void_Tree (Src) or else Sym_Name (Src) /= Name then
         return False;
      end if;
      Reg := D (XD_REGION, Src);
      if Is_Void_Tree (Reg) or else KIND (Reg) /= DN_PACKAGE_ID then
         return False;
      end if;
      if Sym_Name (Reg) = "_STANDRD" then
         return True;
      elsif Sym_Name (Reg) = "STANDARD" then
         declare
            Parent_R : constant TREE := D (XD_REGION, Reg);
         begin
            return Is_Void_Tree (Parent_R) or else Sym_Name (Parent_R) = "_STANDRD";
         end;
      end if;
      return False;
   end Is_Standard_Root_Enum;

   function Is_Boolean_Type (T : TREE) return Boolean is
   begin
      return Is_Standard_Root_Enum (T, "BOOLEAN");
   end Is_Boolean_Type;

   function Is_Character_Type (T : TREE) return Boolean is
   begin
      return Is_Standard_Root_Enum (T, "CHARACTER");
   end Is_Character_Type;

   function Is_Discrete_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_INTEGER or else KIND (R) = DN_ENUMERATION
        or else KIND (R) = DN_UNIVERSAL_INTEGER or else KIND (R) = DN_ANY_INTEGER;
   end Is_Discrete_Type;

   function Is_Integer_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_INTEGER or else KIND (R) = DN_UNIVERSAL_INTEGER
        or else KIND (R) = DN_ANY_INTEGER;
   end Is_Integer_Type;

   function Is_Float_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_FLOAT or else KIND (R) = DN_UNIVERSAL_REAL
        or else KIND (R) = DN_ANY_REAL;
   end Is_Float_Type;

   function Is_Fixed_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_FIXED or else KIND (R) = DN_UNIVERSAL_FIXED;
   end Is_Fixed_Type;

   function Is_Access_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_ACCESS or else KIND (R) = DN_CONSTRAINED_ACCESS;
   end Is_Access_Type;

   function Is_Scalar_Type (T : TREE) return Boolean is
   begin
      return Is_Discrete_Type (T) or else Is_Float_Type (T)
        or else Is_Fixed_Type (T) or else Is_Access_Type (T);
   end Is_Scalar_Type;

   function Is_Record_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_RECORD or else KIND (R) = DN_CONSTRAINED_RECORD;
   end Is_Record_Type;

   function Is_Task_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_TASK_SPEC;
   end Is_Task_Type;

   function Is_Constrained_Array (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      return not Is_Void_Tree (R) and then KIND (R) = DN_CONSTRAINED_ARRAY;
   end Is_Constrained_Array;

   function Is_Unconstrained_Array (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      return not Is_Void_Tree (R) and then KIND (R) = DN_ARRAY;
   end Is_Unconstrained_Array;

   function Is_Array_Type (T : TREE) return Boolean is
   begin
      return Is_Constrained_Array (T) or else Is_Unconstrained_Array (T);
   end Is_Array_Type;

   function Is_String_Type (T : TREE) return Boolean is
      B : constant TREE := Base_Type_Spec (T);
   begin
      if Is_Void_Tree (B) or else KIND (B) /= DN_ARRAY then
         return False;
      end if;
      declare
         Idx_S : constant TREE := D (SM_INDEX_S, B);
         IL    : SEQ_TYPE;
         Dim   : Natural := 0;
         Tmp   : TREE;
      begin
         if Is_Void_Tree (Idx_S) then
            return False;
         end if;
         IL := LIST (Idx_S);
         while not IS_EMPTY (IL) loop
            POP (IL, Tmp);
            Dim := Dim + 1;
            if Dim > 1 then
               return False;
            end if;
         end loop;
         if Dim /= 1 then
            return False;
         end if;
      end;
      return Is_Character_Type (D (SM_COMP_TYPE, B));
   end Is_String_Type;

   function Safe_Impl_Size (T : TREE) return Integer_64 is
      N : TREE;
      V : Integer_64;
      O : Boolean;
   begin
      if Is_Void_Tree (T) then
         return 0;
      end if;
      N := D (CD_IMPL_SIZE, T);
      if Is_Void_Tree (N) then
         return 0;
      end if;
      if KIND (N) = DN_NUM_VAL then
         Eval_Int64_Checked (N, V, O);
         if O or else V < 0 then
            return 0;
         end if;
         return V;
      end if;
      return 0;
   end Safe_Impl_Size;

   function Scalar_Bits (T : TREE) return Natural is
      R : constant TREE := Resolve_Type_Spec (T);
      B : constant TREE := Base_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return 32;
      end if;
      case KIND (R) is
         when DN_ACCESS | DN_CONSTRAINED_ACCESS =>
            return 64;
         when DN_UNIVERSAL_INTEGER | DN_ANY_INTEGER =>
            return 64;
         when DN_UNIVERSAL_REAL | DN_ANY_REAL | DN_UNIVERSAL_FIXED =>
            return 64;
         when DN_ENUMERATION =>
            declare
               Sz : Integer_64 := 0;
            begin
               if not Is_Void_Tree (B) and then KIND (B) = DN_ENUMERATION then
                  Sz := Safe_Impl_Size (B);
               end if;
               if Sz <= 8 then return 8;
               elsif Sz <= 16 then return 16;
               else return 32;
               end if;
            end;
         when DN_INTEGER =>
            declare
               Sz : Integer_64 := Safe_Impl_Size (R);
            begin
               if Sz = 0 and then not Is_Void_Tree (B) and then KIND (B) = DN_INTEGER then
                  Sz := Safe_Impl_Size (B);
               end if;
               if Sz <= 0 then
                  -- Infer from base type range
                  declare
                     St : Boolean;
                     Lo, Hi : Integer_64;
                  begin
                     Get_Discrete_Bounds (B, St, Lo, Hi);
                     if St then
                        if Lo >= -32768 and then Hi <= 32767 then return 16;
                        elsif Lo >= -2147483648 and then Hi <= 2147483647 then return 32;
                        else return 64;
                        end if;
                     end if;
                  end;
                  return 32;
               elsif Sz <= 8 then return 8;
               elsif Sz <= 16 then return 16;
               elsif Sz <= 32 then return 32;
               else return 64;
               end if;
            end;
         when DN_FLOAT =>
            declare
               Sz : Integer_64 := Safe_Impl_Size (R);
            begin
               if Sz = 0 and then not Is_Void_Tree (B) and then KIND (B) = DN_FLOAT then
                  Sz := Safe_Impl_Size (B);
               end if;
               if Sz > 0 then
                  if Sz <= 32 then return 32; else return 64; end if;
               end if;
               declare
                  Acc : constant TREE := D (SM_ACCURACY, R);
               begin
                  if Has_Static_Val (Acc) and then Eval_Int64 (Acc) <= 6 then
                     return 32;
                  end if;
               end;
               return 64;
            end;
         when DN_FIXED =>
            declare
               Sz : Integer_64 := Safe_Impl_Size (R);
            begin
               if Sz = 0 and then not Is_Void_Tree (B) and then KIND (B) = DN_FIXED then
                  Sz := Safe_Impl_Size (B);
               end if;
               if Sz > 0 and then Sz <= 32 then
                  return 32;
               end if;
               return 64;
            end;
         when others =>
            return 64;
      end case;
   end Scalar_Bits;

   function Pir_Scalar_Type (T : TREE) return String is
      R : constant TREE := Resolve_Type_Spec (T);
      Bits : constant Natural := Scalar_Bits (R);
   begin
      if Is_Void_Tree (R) then
         return "i32";
      end if;
      if Is_Access_Type (R) then
         return "ptr";
      elsif Is_Float_Type (R) then
         if Bits <= 32 then return "f32"; else return "f64"; end if;
      else
         case Bits is
            when 8  => return "i8";
            when 16 => return "i16";
            when 32 => return "i32";
            when others => return "i64";
         end case;
      end if;
   end Pir_Scalar_Type;

   function Pow2_Floor (V : IEEE_Float_64) return IEEE_Float_64 is
      Bits : constant Unsigned_64 := To_U64 (V);
      Sign : constant Unsigned_64 := Shift_Right (Bits, 63) and 1;
      Exp  : constant Unsigned_64 := Shift_Right (Bits, 52) and 16#7FF#;
      Frac : constant Unsigned_64 := Bits and 16#000F_FFFF_FFFF_FFFF#;
   begin
      if Sign /= 0 or else (Exp = 0 and then Frac = 0) or else Exp = 16#7FF# then
         return 1.0;
      end if;
      if Exp > 0 then
         -- Normal positive finite f64: drop mantissa bits -> exact floor power of 2 in O(1)
         return To_F64 (Shift_Left (Exp, 52));
      else
         -- Positive subnormal: keep only the highest set bit in Frac
         for B in reverse 0 .. 51 loop
            if (Frac and Shift_Left (Unsigned_64 (1), B)) /= 0 then
               return To_F64 (Shift_Left (Unsigned_64 (1), B));
            end if;
         end loop;
         return 1.0;
      end if;
   end Pow2_Floor;

   -- Representation tick scale: ALWAYS use Base_Type_Spec(T) first so fixed subtypes
   -- with coarser DELTA still share the base type's scaled-integer representation!
   function Fixed_Small (T : TREE) return IEEE_Float_64 is
      R  : constant TREE := Resolve_Type_Spec (T);
      B  : constant TREE := Base_Type_Spec (T);
      Sm : TREE;
   begin
      if not Is_Void_Tree (B) and then KIND (B) = DN_FIXED then
         Sm := D (CD_IMPL_SMALL, B);
         if Has_Static_Val (Sm) then
            return Eval_Float64 (Sm);
         end if;
      end if;
      if not Is_Void_Tree (R) and then KIND (R) = DN_FIXED then
         Sm := D (CD_IMPL_SMALL, R);
         if Has_Static_Val (Sm) then
            return Eval_Float64 (Sm);
         end if;
      end if;
      if not Is_Void_Tree (B) and then KIND (B) = DN_FIXED then
         Sm := D (SM_ACCURACY, B);
         if Has_Static_Val (Sm) then
            return Pow2_Floor (Eval_Float64 (Sm));
         end if;
      end if;
      if not Is_Void_Tree (R) and then KIND (R) = DN_FIXED then
         Sm := D (SM_ACCURACY, R);
         if Has_Static_Val (Sm) then
            return Pow2_Floor (Eval_Float64 (Sm));
         end if;
      end if;
      return 1.0E-9;
   end Fixed_Small;

   -- Subtype 'SMALL attribute (for 'SMALL, 'MANTISSA, 'LARGE)
   function Fixed_Attr_Small (T : TREE) return IEEE_Float_64 is
      R  : constant TREE := Resolve_Type_Spec (T);
      B  : constant TREE := Base_Type_Spec (T);
      Sm : TREE;
   begin
      if not Is_Void_Tree (R) and then KIND (R) = DN_FIXED then
         Sm := D (CD_IMPL_SMALL, R);
         if Has_Static_Val (Sm) then
            return Eval_Float64 (Sm);
         end if;
         Sm := D (SM_ACCURACY, R);
         if Has_Static_Val (Sm) then
            return Pow2_Floor (Eval_Float64 (Sm));
         end if;
      end if;
      return Fixed_Small (B);
   end Fixed_Attr_Small;

   function Gcd_U64 (A, B : Unsigned_64) return Unsigned_64 is
      X : Unsigned_64 := A;
      Y : Unsigned_64 := B;
      T : Unsigned_64;
   begin
      while Y /= 0 loop
         T := X mod Y;
         X := Y;
         Y := T;
      end loop;
      return (if X = 0 then 1 else X);
   end Gcd_U64;

   procedure Fixed_Small_Rational
     (T   : TREE;
      Ok  : out Boolean;
      Num : out Integer_64;
      Den : out Integer_64)
   is
      R  : constant TREE := Resolve_Type_Spec (T);
      B  : constant TREE := Base_Type_Spec (T);
      Sm : TREE := CONST_VOID;
      O1, O2 : Boolean;
      N, D_V : Integer_64;
   begin
      Ok  := False;
      Num := 1;
      Den := 1;
      if not Is_Void_Tree (B) and then KIND (B) = DN_FIXED then
         Sm := D (CD_IMPL_SMALL, B);
      end if;
      if (Is_Void_Tree (Sm) or else KIND (Sm) /= DN_REAL_VAL)
        and then not Is_Void_Tree (R) and then KIND (R) = DN_FIXED
      then
         Sm := D (CD_IMPL_SMALL, R);
      end if;
      if not Is_Void_Tree (Sm) and then KIND (Sm) = DN_REAL_VAL then
         Eval_Int64_Checked (D (XD_NUMER, Sm), N, O1);
         Eval_Int64_Checked (D (XD_DENOM, Sm), D_V, O2);
         if not O1 and then not O2 and then N > 0 and then D_V > 0 then
            declare
               G : constant Unsigned_64 := Gcd_U64 (Unsigned_64 (N), Unsigned_64 (D_V));
            begin
               Num := Integer_64 (Unsigned_64 (N) / G);
               Den := Integer_64 (Unsigned_64 (D_V) / G);
               Ok  := True;
            end;
         end if;
      end if;
   end Fixed_Small_Rational;

   procedure Eval_Fixed_Ticks_Exact_Checked
     (SV      : TREE;
      Fixed_T : TREE;
      Val     : out Integer_64;
      Ovf     : out Boolean)
   is
      BT   : constant TREE := Base_Type_Spec (Fixed_T);
      Bits : constant Natural := Scalar_Bits (if not Is_Void_Tree (BT) then BT else Fixed_T);
      Min_I : constant Integer_64 :=
        (if Bits >= 64 then Integer_64'First
         else -(Integer_64 (Shift_Left (Unsigned_64 (1), Bits - 1))));
      Max_I : constant Integer_64 :=
        (if Bits >= 64 then Integer_64'Last
         else Integer_64 (Shift_Left (Unsigned_64 (1), Bits - 1) - 1));
      Sm_Ok : Boolean;
      Sm_N, Sm_D : Integer_64;
      Used_Exact : Boolean := False;
   begin
      Val := 0;
      Ovf := False;
      Fixed_Small_Rational (Fixed_T, Sm_Ok, Sm_N, Sm_D);
      if Sm_Ok and then not Is_Void_Tree (SV) and then (KIND (SV) = DN_REAL_VAL or else KIND (SV) = DN_NUM_VAL) then
         declare
            VN, VD : Integer_64 := 1;
            ON, OD : Boolean := False;
         begin
            if KIND (SV) = DN_NUM_VAL then
               Eval_Int64_Checked (SV, VN, ON);
               VD := 1;
               OD := False;
            else
               Eval_Int64_Checked (D (XD_NUMER, SV), VN, ON);
               Eval_Int64_Checked (D (XD_DENOM, SV), VD, OD);
            end if;
            if not ON and then not OD and then VD > 0 then
               declare
                  Neg : constant Boolean := VN < 0;
                  UN  : Unsigned_64 := (if VN = Integer_64'First then Unsigned_64 (Integer_64'Last) + 1
                                        elsif VN < 0 then Unsigned_64 (-VN)
                                        else Unsigned_64 (VN));
                  UD  : Unsigned_64 := Unsigned_64 (VD);
                  USN : Unsigned_64 := Unsigned_64 (Sm_N);
                  USD : Unsigned_64 := Unsigned_64 (Sm_D);
                  G1, G2 : Unsigned_64;
               begin
                  -- Reduce (UN / UD) / (USN / USD) = (UN * USD) / (UD * USN)
                  G1 := Gcd_U64 (UN, USN); UN := UN / G1; USN := USN / G1;
                  G2 := Gcd_U64 (USD, UD); USD := USD / G2; UD := UD / G2;
                  if (USD = 0 or else UN <= Unsigned_64'Last / USD)
                    and then (USN = 0 or else UD <= Unsigned_64'Last / USN)
                  then
                     declare
                        Num_P : constant Unsigned_64 := UN * USD;
                        Den_P : constant Unsigned_64 := UD * USN;
                        Q     : Unsigned_64;
                        Rem_V : Unsigned_64;
                     begin
                        if Den_P > 0 then
                           Q := Num_P / Den_P;
                           Rem_V := Num_P mod Den_P;
                           if Rem_V >= Den_P - Rem_V then
                              if Q < Unsigned_64'Last then Q := Q + 1; else Ovf := True; end if;
                           end if;
                           Used_Exact := True;
                           if Ovf then
                              Val := (if Neg then Min_I else Max_I);
                           else
                              if not Neg then
                                 if Bits >= 64 and then Q = Unsigned_64 (Integer_64'Last) + 1 then
                                    Val := Max_I;
                                 elsif Q > Unsigned_64 (Max_I) then
                                    Ovf := True; Val := Max_I;
                                 else
                                    Val := Integer_64 (Q);
                                 end if;
                              else
                                 declare
                                    Max_Neg : constant Unsigned_64 :=
                                      (if Min_I = Integer_64'First then Unsigned_64 (Integer_64'Last) + 1
                                       else Unsigned_64 (-Min_I));
                                 begin
                                    if Q > Max_Neg then
                                       Ovf := True; Val := Min_I;
                                    elsif Q = Unsigned_64 (Integer_64'Last) + 1 then
                                       Val := Integer_64'First;
                                    else
                                       Val := -Integer_64 (Q);
                                    end if;
                                 end;
                              end if;
                           end if;
                        end if;
                     end;
                  end if;
               end;
            end if;
         end;
      end if;
      if not Used_Exact then
         declare
            Sm    : constant IEEE_Float_64 := Fixed_Small (Fixed_T);
            FVal  : constant IEEE_Float_64 := Eval_Float64 (SV);
            Rat   : constant IEEE_Float_64 := (if Sm > 0.0 then FVal / Sm else 0.0);
            Min_F : constant IEEE_Float_64 := IEEE_Float_64 (Min_I);
            Max_F : constant IEEE_Float_64 := IEEE_Float_64 (Max_I);
            Rnd   : IEEE_Float_64;
         begin
            if not (Rat = Rat) then
               Ovf := True;
               Val := 0;
            else
               Rnd := IEEE_Float_64'Rounding (Rat);
               if Rnd < Min_F then
                  Ovf := True; Val := Min_I;
               elsif Bits >= 64 and then Rnd > 9223372036854775808.0 then
                  Ovf := True; Val := Max_I;
               elsif Bits < 64 and then Rnd > Max_F then
                  Ovf := True; Val := Max_I;
               elsif Rnd >= 9223372036854775808.0 then
                  Val := Max_I;
               elsif Rnd <= -9223372036854775808.0 then
                  Val := Min_I;
               else
                  Val := Integer_64 (Rnd);
                  if Val < Min_I then
                     Ovf := True; Val := Min_I;
                  elsif Val > Max_I then
                     Ovf := True; Val := Max_I;
                  end if;
               end if;
            end if;
         end;
      end if;
   end Eval_Fixed_Ticks_Exact_Checked;

   -- Subtype 'DELTA attribute
   function Fixed_Delta (T : TREE) return IEEE_Float_64 is
      R  : constant TREE := Resolve_Type_Spec (T);
      B  : constant TREE := Base_Type_Spec (T);
      Ac : TREE;
   begin
      if not Is_Void_Tree (R) and then KIND (R) = DN_FIXED then
         Ac := D (SM_ACCURACY, R);
         if Has_Static_Val (Ac) then
            return Eval_Float64 (Ac);
         end if;
      end if;
      if not Is_Void_Tree (B) and then KIND (B) = DN_FIXED then
         Ac := D (SM_ACCURACY, B);
         if Has_Static_Val (Ac) then
            return Eval_Float64 (Ac);
         end if;
      end if;
      return Fixed_Small (T);
   end Fixed_Delta;

   procedure Get_Discrete_Bounds
     (T      : TREE;
      Static : out Boolean;
      First  : out Integer_64;
      Last   : out Integer_64)
   is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      Static := False;
      First := 0;
      Last := 0;
      if Is_Void_Tree (R) then
         return;
      end if;
      if KIND (R) = DN_INTEGER or else KIND (R) = DN_ENUMERATION then
         declare
            Rng : constant TREE := D (SM_RANGE, R);
         begin
            if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
               declare
                  E1 : constant TREE := D (AS_EXP1, Rng);
                  E2 : constant TREE := D (AS_EXP2, Rng);
                  V1 : constant TREE := Safe_SM_Value (E1);
                  V2 : constant TREE := Safe_SM_Value (E2);
               begin
                  if Has_Static_Val (V1) and then Has_Static_Val (V2) then
                     Static := True;
                     First := Eval_Int64 (V1);
                     Last  := Eval_Int64 (V2);
                     return;
                  end if;
               end;
            end if;
         end;
      end if;
   end Get_Discrete_Bounds;

   function Enclosing_Package_Name (Id : TREE) return String is
      Cur : TREE := Id;
   begin
      for Depth in 1 .. 16 loop
         exit when Is_Void_Tree (Cur);
         if KIND (Cur) in CLASS_SOURCE_NAME then
            declare
               Reg : constant TREE := D (XD_REGION, Cur);
            begin
               exit when Is_Void_Tree (Reg);
               if KIND (Reg) = DN_PACKAGE_ID then
                  declare
                     N : constant String := Sym_Name (Reg);
                  begin
                     if N /= "_STANDRD" then
                        return N;
                     end if;
                  end;
               end if;
               Cur := Reg;
            end;
         else
            exit;
         end if;
      end loop;
      return "";
   end Enclosing_Package_Name;

   function Is_Predefined_Standard_Pkg (Pkg_Id : TREE) return Boolean is
   begin
      if Is_Void_Tree (Pkg_Id) or else KIND (Pkg_Id) /= DN_PACKAGE_ID then
         return False;
      end if;
      declare
         N : constant String := Sym_Name (Pkg_Id);
      begin
         if N = "_STANDRD" then
            return True;
         elsif N = "STANDARD" then
            declare
               Reg : constant TREE := D (XD_REGION, Pkg_Id);
            begin
               return Is_Void_Tree (Reg) or else Sym_Name (Reg) = "_STANDRD";
            end;
         end if;
      end;
      return False;
   end Is_Predefined_Standard_Pkg;

   function Qualified_Ada_Name (Id : TREE) return String is
      Parts : Unbounded_String;
      Cur   : TREE := Canonical_Def (Id);
   begin
      for Depth in 1 .. 16 loop
         exit when Is_Void_Tree (Cur);
         declare
            N : constant String := Sym_Name (Cur);
         begin
            if N'Length > 0 and then N /= "_STANDRD" and then not Is_Predefined_Standard_Pkg (Cur) then
               if Length (Parts) = 0 then
                  Parts := To_Unbounded_String (Sanitize_Ident (N));
               else
                  Parts := To_Unbounded_String (Sanitize_Ident (N) & "__") & Parts;
               end if;
            end if;
         end;
         if KIND (Cur) in CLASS_SOURCE_NAME then
            Cur := D (XD_REGION, Cur);
         else
            exit;
         end if;
      end loop;
      return To_String (Parts);
   end Qualified_Ada_Name;

   function Is_Library_Level (Id : TREE) return Boolean is
      Cur : TREE := Canonical_Def (Id);
   begin
      if Is_Void_Tree (Cur) or else KIND (Cur) not in CLASS_SOURCE_NAME then
         return False;
      end if;
      Cur := D (XD_REGION, Cur);
      for Depth in 1 .. 16 loop
         if Is_Void_Tree (Cur) then
            return True;
         end if;
         if KIND (Cur) = DN_PACKAGE_ID then
            if Is_Predefined_Standard_Pkg (Cur) then
               return True;
            end if;
            Cur := D (XD_REGION, Cur);
         else
            return False;
         end if;
      end loop;
      return False;
   end Is_Library_Level;

   function Is_Unsigned_Scalar (T : TREE; Pir_Type : String) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      if KIND (R) = DN_ENUMERATION then
         return True;
      end if;
      if KIND (R) = DN_INTEGER then
         declare
            function Fits_Unsigned (Node : TREE) return Boolean is
               St     : Boolean := False;
               Lo, Hi : Integer_64 := 0;
            begin
               if Is_Void_Tree (Node) then
                  return False;
               end if;
               Get_Discrete_Bounds (Node, St, Lo, Hi);
               if St and then Lo >= 0 then
                  if Pir_Type = "i8" and then Hi > 127 then
                     return True;
                  elsif Pir_Type = "i16" and then Hi > 32767 then
                     return True;
                  elsif Pir_Type = "i32" and then Hi > 2147483647 then
                     return True;
                  end if;
               end if;
               return False;
            end Fits_Unsigned;
            St_R : Boolean := False;
            Lo_R, Hi_R : Integer_64 := 0;
         begin
            -- Check subtype R first (handles `type U8 is range 0..255; for U8'Size use 8;` where
            -- DIANA's anonymous base type is signed -32768..32767), then fall back to base type
            -- when R is non-negative or dynamic.
            if Fits_Unsigned (R) then
               return True;
            end if;
            Get_Discrete_Bounds (R, St_R, Lo_R, Hi_R);
            if not St_R or else Lo_R >= 0 then
               return Fits_Unsigned (Base_Type_Spec (R));
            end if;
         end;
      end if;
      return False;
   end Is_Unsigned_Scalar;

   function Decode_Ada_String_Lit (Raw : String) return String is
      Res : Unbounded_String;
      I   : Integer := Raw'First;
      Fin : Integer := Raw'Last;
   begin
      if Raw'Length >= 2 and then Raw (I) = '"' and then Raw (Fin) = '"' then
         I := I + 1;
         Fin := Fin - 1;
      end if;
      while I <= Fin loop
         if Raw (I) = '"' and then I + 1 <= Fin and then Raw (I + 1) = '"' then
            Append (Res, '"');
            I := I + 2;
         else
            Append (Res, Raw (I));
            I := I + 1;
         end if;
      end loop;
      return To_String (Res);
   end Decode_Ada_String_Lit;

end Diana_Util;
