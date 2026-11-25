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
                  if KIND (Target) = DN_SELECTED then
                     Target := D (AS_DESIGNATOR, Target);
                  end if;
                  if not Is_Void_Tree (Target) then
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
            | DN_FUNCTION_CALL | DN_NULLARY_CALL
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
         declare
            F : constant IEEE_Float_64 := Eval_Float64 (Val_Node);
         begin
            if F > IEEE_Float_64 (Integer_64'Last) then
               Val := Integer_64'Last;
               Overflowed := True;
            elsif F < IEEE_Float_64 (Integer_64'First) then
               Val := Integer_64'First;
               Overflowed := True;
            else
               Val := Integer_64 (F);
            end if;
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
         Hdr   : constant TREE := DABS (0, Val_Node);
         Wlen  : constant Integer := Integer (Hdr.TLN);
         Digs  : array (1 .. 512) of Integer;
         Ndig  : Integer := 2 * Wlen;
         Neg   : Boolean := False;
         Acc   : Unsigned_64 := 0;
         Ovf   : Boolean := False;
      begin
         for J in 1 .. Wlen loop
            declare
               Pair : constant UDigit_Pair := To_UDigit_Pair (DABS (J, Val_Node));
            begin
               Digs (J * 2 - 1) := Integer (Pair.D1);
               Digs (J * 2)     := Integer (Pair.D2);
            end;
         end loop;
         if Digs (1) >= 10000 then
            Digs (1) := Digs (1) - 10000;
            Neg := True;
         end if;
         while Ndig > 1 and then Digs (Ndig) = 0 loop
            Ndig := Ndig - 1;
         end loop;
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

   -- Decode a DN_NUM_VAL into an exact decimal string for 0.5-ULP IEEE_Float_64'Value parsing
   function Num_Val_To_Decimal_Str (Num_Node : TREE) return String is
   begin
      if Is_Void_Tree (Num_Node) or else KIND (Num_Node) /= DN_NUM_VAL then
         return "0.0";
      end if;
      if Num_Node.TLN = 0 then
         declare
            S : constant String := Ada.Strings.Fixed.Trim (Integer'Image (Integer (Num_Node.TPG)), Ada.Strings.Both);
         begin
            return S & ".0";
         end;
      end if;
      declare
         Hdr  : constant TREE := DABS (0, Num_Node);
         Wlen : constant Integer := Integer (Hdr.TLN);
         Digs : array (1 .. 512) of Integer;
         Ndig : Integer := 2 * Wlen;
         Neg  : Boolean := False;
         Low  : Integer;
         Buf  : String (1 .. 64);
         Pos  : Natural := 0;
         procedure Put_Ch (C : Character) is
         begin
            Pos := Pos + 1;
            Buf (Pos) := C;
         end Put_Ch;
      begin
         for J in 1 .. Wlen loop
            declare
               Pair : constant UDigit_Pair := To_UDigit_Pair (DABS (J, Num_Node));
            begin
               Digs (J * 2 - 1) := Integer (Pair.D1);
               Digs (J * 2)     := Integer (Pair.D2);
            end;
         end loop;
         if Digs (1) >= 10000 then
            Digs (1) := Digs (1) - 10000;
            Neg := True;
         end if;
         while Ndig > 1 and then Digs (Ndig) = 0 loop
            Ndig := Ndig - 1;
         end loop;
         if Neg then
            Put_Ch ('-');
         end if;
         -- Top 6 base-10000 digits (21..24 decimal digits > 17 f64 digits)
         Low := Integer'Max (1, Ndig - 5);
         declare
            Top : constant String := Ada.Strings.Fixed.Trim (Integer'Image (Digs (Ndig)), Ada.Strings.Both);
         begin
            for C of Top loop
               Put_Ch (C);
            end loop;
         end;
         for I in reverse Low .. Ndig - 1 loop
            declare
               V : constant Integer := Digs (I);
            begin
               Put_Ch (Character'Val (Character'Pos ('0') + (V / 1000) mod 10));
               Put_Ch (Character'Val (Character'Pos ('0') + (V / 100) mod 10));
               Put_Ch (Character'Val (Character'Pos ('0') + (V / 10) mod 10));
               Put_Ch (Character'Val (Character'Pos ('0') + V mod 10));
            end;
         end loop;
         Put_Ch ('.');
         Put_Ch ('0');
         if Low > 1 then
            declare
               Exp_S : constant String := Ada.Strings.Fixed.Trim (Integer'Image ((Low - 1) * 4), Ada.Strings.Both);
            begin
               Put_Ch ('E');
               for C of Exp_S loop
                  Put_Ch (C);
               end loop;
            end;
         end if;
         return Buf (1 .. Pos);
      end;
   end Num_Val_To_Decimal_Str;

   -- Decode a DN_NUM_VAL into (mantissa as f64, decimal exponent base 10000)
   procedure Decode_Big_Float
     (Num_Node : TREE;
      Mant     : out IEEE_Float_64;
      Exp10k   : out Integer)
   is
   begin
      if Is_Void_Tree (Num_Node) or else KIND (Num_Node) /= DN_NUM_VAL then
         Mant := 0.0;
         Exp10k := 0;
         return;
      end if;
      if Num_Node.TLN = 0 then
         Mant := IEEE_Float_64 (Num_Node.TPG);
         Exp10k := 0;
         return;
      end if;
      declare
         Hdr  : constant TREE := DABS (0, Num_Node);
         Wlen : constant Integer := Integer (Hdr.TLN);
         Digs : array (1 .. 512) of Integer;
         Ndig : Integer := 2 * Wlen;
         Neg  : Boolean := False;
         Low  : Integer;
         Buf  : String (1 .. 48);
         Pos  : Natural := 0;
         procedure Put_Ch (C : Character) is
         begin
            Pos := Pos + 1;
            Buf (Pos) := C;
         end Put_Ch;
      begin
         for J in 1 .. Wlen loop
            declare
               Pair : constant UDigit_Pair := To_UDigit_Pair (DABS (J, Num_Node));
            begin
               Digs (J * 2 - 1) := Integer (Pair.D1);
               Digs (J * 2)     := Integer (Pair.D2);
            end;
         end loop;
         if Digs (1) >= 10000 then
            Digs (1) := Digs (1) - 10000;
            Neg := True;
         end if;
         while Ndig > 1 and then Digs (Ndig) = 0 loop
            Ndig := Ndig - 1;
         end loop;
         Low := Integer'Max (1, Ndig - 5);
         if Neg then
            Put_Ch ('-');
         end if;
         declare
            Top : constant String := Ada.Strings.Fixed.Trim (Integer'Image (Digs (Ndig)), Ada.Strings.Both);
         begin
            for C of Top loop
               Put_Ch (C);
            end loop;
         end;
         for I in reverse Low .. Ndig - 1 loop
            declare
               V : constant Integer := Digs (I);
            begin
               Put_Ch (Character'Val (Character'Pos ('0') + (V / 1000) mod 10));
               Put_Ch (Character'Val (Character'Pos ('0') + (V / 100) mod 10));
               Put_Ch (Character'Val (Character'Pos ('0') + (V / 10) mod 10));
               Put_Ch (Character'Val (Character'Pos ('0') + V mod 10));
            end;
         end loop;
         Put_Ch ('.');
         Put_Ch ('0');
         Mant := IEEE_Float_64'Value (Buf (1 .. Pos));
         Exp10k := Low - 1;
      end;
   end Decode_Big_Float;

   function Eval_Float64 (Val_Node : TREE) return IEEE_Float_64 is
   begin
      if Is_Void_Tree (Val_Node) then
         return 0.0;
      end if;
      if KIND (Val_Node) = DN_NUM_VAL then
         begin
            return IEEE_Float_64'Value (Num_Val_To_Decimal_Str (Val_Node));
         exception
            when others =>
               return 0.0;
         end;
      end if;
      if KIND (Val_Node) = DN_REAL_VAL then
         declare
            NNode : constant TREE := D (XD_NUMER, Val_Node);
            DNode : constant TREE := D (XD_DENOM, Val_Node);
         begin
            -- Fast exact path when denominator is 1 (e.g. large powers of 2 / hex float literals)
            if not Is_Void_Tree (DNode) and then KIND (DNode) = DN_NUM_VAL
              and then DNode.TLN = 0 and then DNode.TPG = 1
            then
               begin
                  return IEEE_Float_64'Value (Num_Val_To_Decimal_Str (NNode));
               exception
                  when others =>
                     null;
               end;
            end if;
            -- Exact arbitrary-precision binary division on base-10000 limbs -> 0.5-ULP IEEE-754 f64
            declare
               Max_Limbs : constant Natural := 512;
               type Big_Arr is array (1 .. Max_Limbs) of Natural;
               N_Digs, D_Digs : Big_Arr := (others => 0);
               N_Len, D_Len   : Natural := 1;
               N_Neg, D_Neg   : Boolean := False;
               Neg            : Boolean;

               procedure Decode_Limbs (Node : TREE; A : out Big_Arr; Len : out Natural; Is_Neg : out Boolean) is
               begin
                  A := (others => 0);
                  Len := 1;
                  Is_Neg := False;
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
                        A (1) := V mod 10000;
                        if V >= 10000 then
                           A (2) := V / 10000;
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
                     for J in 1 .. Wlen loop
                        declare
                           Pair : constant UDigit_Pair := To_UDigit_Pair (DABS (J, Node));
                        begin
                           if J * 2 - 1 <= Max_Limbs then
                              A (J * 2 - 1) := Natural (Pair.D1);
                           end if;
                           if J * 2 <= Max_Limbs then
                              A (J * 2) := Natural (Pair.D2);
                           end if;
                        end;
                     end loop;
                     if A (1) >= 10000 then
                        A (1) := A (1) - 10000;
                        Is_Neg := True;
                     end if;
                     if ND > Max_Limbs then ND := Max_Limbs; end if;
                     while ND > 1 and then A (ND) = 0 loop
                        ND := ND - 1;
                     end loop;
                     Len := Natural (ND);
                  end;
               end Decode_Limbs;

               function Cmp_Big (A : Big_Arr; AL : Natural; B : Big_Arr; BL : Natural) return Integer is
               begin
                  if AL < BL then return -1; end if;
                  if AL > BL then return 1; end if;
                  for I in reverse 1 .. AL loop
                     if A (I) < B (I) then return -1; end if;
                     if A (I) > B (I) then return 1; end if;
                  end loop;
                  return 0;
               end Cmp_Big;

               procedure Mul2_Big (A : in out Big_Arr; AL : in out Natural) is
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
                  if Carry > 0 and then AL < Max_Limbs then
                     AL := AL + 1;
                     A (AL) := Carry;
                  end if;
               end Mul2_Big;

               procedure Sub_Big (A : in out Big_Arr; AL : in out Natural; B : Big_Arr; BL : Natural) is
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

               Exp        : Integer := 0;
               Bits       : Integer;
               Biased_Exp : Integer;
               Q          : Unsigned_64 := 0;
               Mant_Bits  : Unsigned_64 := 0;
               C          : Integer;
               Bits64     : Unsigned_64;
            begin
               Decode_Limbs (NNode, N_Digs, N_Len, N_Neg);
               Decode_Limbs (DNode, D_Digs, D_Len, D_Neg);
               Neg := N_Neg xor D_Neg;
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
            end;
         end;
      end if;
      return 0.0;
   end Eval_Float64;

   function Format_Float64 (V : IEEE_Float_64) return String is
      S : constant String := Ada.Strings.Fixed.Trim (IEEE_Float_64'Image (V), Ada.Strings.Both);
   begin
      if S'Length >= 4 and then (S (S'First .. S'First + 3) = "+Inf" or else S (S'First .. S'First + 2) = "Inf") then
         return "inf";
      elsif S'Length >= 4 and then S (S'First .. S'First + 3) = "-Inf" then
         return "-inf";
      elsif S'Length >= 3 and then (S (S'First .. S'First + 2) = "NaN" or else S (S'First .. S'First + 2) = "Nan") then
         return "nan";
      end if;
      -- Print full 17 significant digits (Aft => 16) for exact IEEE-754 f64 round-trip!
      declare
         Buf : String (1 .. 32);
      begin
         Ada.Long_Float_Text_IO.Put (Buf, Long_Float (V), Aft => 16, Exp => 3);
         return Ada.Strings.Fixed.Trim (Buf, Ada.Strings.Both);
      exception
         when others =>
            if Ada.Strings.Fixed.Index (S, ".") = 0 and then Ada.Strings.Fixed.Index (S, "E") = 0 then
               return S & ".0";
            end if;
            return S;
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
      return Is_Character_Type (D (SM_COMP_TYPE, B));
   end Is_String_Type;

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
               Sz : Integer := 0;
            begin
               if not Is_Void_Tree (B) and then KIND (B) = DN_ENUMERATION then
                  Sz := DI (CD_IMPL_SIZE, B);
               end if;
               if Sz <= 8 then return 8;
               elsif Sz <= 16 then return 16;
               else return 32;
               end if;
            end;
         when DN_INTEGER =>
            declare
               Sz : Integer := DI (CD_IMPL_SIZE, R);
            begin
               if Sz = 0 and then not Is_Void_Tree (B) and then KIND (B) = DN_INTEGER then
                  Sz := DI (CD_IMPL_SIZE, B);
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
               Sz : Integer := DI (CD_IMPL_SIZE, R);
            begin
               if Sz = 0 and then not Is_Void_Tree (B) and then KIND (B) = DN_FLOAT then
                  Sz := DI (CD_IMPL_SIZE, B);
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
               Sz : Integer := DI (CD_IMPL_SIZE, R);
            begin
               if Sz = 0 and then not Is_Void_Tree (B) and then KIND (B) = DN_FIXED then
                  Sz := DI (CD_IMPL_SIZE, B);
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
      P : IEEE_Float_64 := 1.0;
   begin
      if V <= 0.0 then
         return 1.0;
      end if;
      if V >= 1.0 then
         while P * 2.0 <= V loop
            P := P * 2.0;
         end loop;
      else
         while P > V loop
            P := P * 0.5;
         end loop;
      end if;
      return P;
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
         exit when Is_Void_Tree (Cur);
         if KIND (Cur) = DN_PACKAGE_ID then
            if Is_Predefined_Standard_Pkg (Cur) then
               return True;
            end if;
            Cur := D (XD_REGION, Cur);
         else
            return False;
         end if;
      end loop;
      return True;
   end Is_Library_Level;

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
