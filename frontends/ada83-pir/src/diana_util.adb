-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Characters.Handling;
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
            | DN_TYPE_ID | DN_CONSTANT_ID =>
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
               begin
                  if not Is_Void_Tree (Full) then
                     Cur := Full;
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

   function Has_Static_Val (T : TREE) return Boolean is
   begin
      if Is_Void_Tree (T) then
         return False;
      end if;
      return KIND (T) = DN_NUM_VAL or else KIND (T) = DN_REAL_VAL;
   end Has_Static_Val;

   function Eval_Int64 (Val_Node : TREE) return Integer_64 is
   begin
      if Is_Void_Tree (Val_Node) then
         return 0;
      end if;
      if KIND (Val_Node) = DN_REAL_VAL then
         return Integer_64 (Eval_Float64 (Val_Node));
      end if;
      if KIND (Val_Node) /= DN_NUM_VAL then
         return 0;
      end if;
      if Val_Node.TLN = 0 then
         return Integer_64 (Val_Node.TPG);
      end if;
      declare
         Hdr   : constant TREE := DABS (0, Val_Node);
         Wlen  : constant Integer := Integer (Hdr.TLN);
         Digs  : array (1 .. 512) of Integer;
         Ndig  : Integer := 2 * Wlen;
         Neg   : Boolean := False;
         Acc   : Unsigned_64 := 0;
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
            Acc := Acc * 10000 + Unsigned_64 (Digs (I));
         end loop;
         if Neg then
            if Acc = 16#8000_0000_0000_0000# then
               return Integer_64'First;
            else
               return -Integer_64 (Acc);
            end if;
         else
            return Integer_64 (Acc);
         end if;
      end;
   end Eval_Int64;

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
         Acc  : IEEE_Float_64 := 0.0;
         Low  : Integer;
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
         -- Take top 8 base-10000 digits (32 decimal digits > f64 17-digit precision)
         Low := Integer'Max (1, Ndig - 7);
         for I in reverse Low .. Ndig loop
            Acc := Acc * 10000.0 + IEEE_Float_64 (Digs (I));
         end loop;
         if Neg then
            Acc := -Acc;
         end if;
         Mant := Acc;
         Exp10k := Low - 1;
      end;
   end Decode_Big_Float;

   function Eval_Float64 (Val_Node : TREE) return IEEE_Float_64 is
   begin
      if Is_Void_Tree (Val_Node) then
         return 0.0;
      end if;
      if KIND (Val_Node) = DN_NUM_VAL then
         declare
            M : IEEE_Float_64;
            E : Integer;
         begin
            Decode_Big_Float (Val_Node, M, E);
            return M * (10000.0 ** E);
         end;
      end if;
      if KIND (Val_Node) = DN_REAL_VAL then
         declare
            Nm, Dm : IEEE_Float_64;
            Ne, De : Integer;
            Ratio  : IEEE_Float_64;
            Dexp   : Integer;
         begin
            Decode_Big_Float (D (XD_NUMER, Val_Node), Nm, Ne);
            Decode_Big_Float (D (XD_DENOM, Val_Node), Dm, De);
            if Dm = 0.0 then
               return 0.0;
            end if;
            Ratio := Nm / Dm;
            Dexp := Ne - De;
            while Dexp > 0 loop
               Ratio := Ratio * 10000.0;
               Dexp := Dexp - 1;
            end loop;
            while Dexp < 0 loop
               Ratio := Ratio / 10000.0;
               Dexp := Dexp + 1;
            end loop;
            return Ratio;
         end;
      end if;
      return 0.0;
   end Eval_Float64;

   function Format_Float64 (V : IEEE_Float_64) return String is
      S : constant String := Ada.Strings.Fixed.Trim (IEEE_Float_64'Image (V), Ada.Strings.Both);
   begin
      -- Ensure it has a '.' or 'E' so PIR lexer parses it as float
      if Ada.Strings.Fixed.Index (S, ".") = 0 and then Ada.Strings.Fixed.Index (S, "E") = 0 then
         return S & ".0";
      end if;
      return S;
   end Format_Float64;

   function Is_Boolean_Type (T : TREE) return Boolean is
      B : constant TREE := Base_Type_Spec (T);
   begin
      if Is_Void_Tree (B) or else KIND (B) /= DN_ENUMERATION then
         return False;
      end if;
      declare
         Src : constant TREE := D (XD_SOURCE_NAME, B);
      begin
         return not Is_Void_Tree (Src) and then Sym_Name (Src) = "BOOLEAN";
      end;
   end Is_Boolean_Type;

   function Is_Character_Type (T : TREE) return Boolean is
      B : constant TREE := Base_Type_Spec (T);
   begin
      if Is_Void_Tree (B) or else KIND (B) /= DN_ENUMERATION then
         return False;
      end if;
      declare
         Src : constant TREE := D (XD_SOURCE_NAME, B);
      begin
         return not Is_Void_Tree (Src) and then Sym_Name (Src) = "CHARACTER";
      end;
   end Is_Character_Type;

   function Is_Discrete_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_INTEGER or else KIND (R) = DN_ENUMERATION
        or else KIND (R) = DN_UNIVERSAL_INTEGER;
   end Is_Discrete_Type;

   function Is_Integer_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_INTEGER or else KIND (R) = DN_UNIVERSAL_INTEGER;
   end Is_Integer_Type;

   function Is_Float_Type (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
   begin
      if Is_Void_Tree (R) then
         return False;
      end if;
      return KIND (R) = DN_FLOAT or else KIND (R) = DN_UNIVERSAL_REAL;
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
         when DN_UNIVERSAL_INTEGER =>
            return 64;
         when DN_UNIVERSAL_REAL | DN_UNIVERSAL_FIXED =>
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

   function Fixed_Small (T : TREE) return IEEE_Float_64 is
      R : constant TREE := Resolve_Type_Spec (T);
      B : constant TREE := Base_Type_Spec (T);
      Sm : TREE;
   begin
      if not Is_Void_Tree (R) and then KIND (R) = DN_FIXED then
         Sm := D (CD_IMPL_SMALL, R);
         if Has_Static_Val (Sm) then
            return Eval_Float64 (Sm);
         end if;
         Sm := D (SM_ACCURACY, R);
         if Has_Static_Val (Sm) then
            return Eval_Float64 (Sm);
         end if;
      end if;
      if not Is_Void_Tree (B) and then KIND (B) = DN_FIXED then
         Sm := D (CD_IMPL_SMALL, B);
         if Has_Static_Val (Sm) then
            return Eval_Float64 (Sm);
         end if;
         Sm := D (SM_ACCURACY, B);
         if Has_Static_Val (Sm) then
            return Eval_Float64 (Sm);
         end if;
      end if;
      return 1.0E-9;
   end Fixed_Small;

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
                  V1 : constant TREE := D (SM_VALUE, E1);
                  V2 : constant TREE := D (SM_VALUE, E2);
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

   function Qualified_Ada_Name (Id : TREE) return String is
      Parts : Unbounded_String;
      Cur   : TREE := Canonical_Def (Id);
   begin
      for Depth in 1 .. 16 loop
         exit when Is_Void_Tree (Cur);
         declare
            N : constant String := Sym_Name (Cur);
         begin
            if N'Length > 0 and then N /= "_STANDRD" and then N /= "STANDARD" then
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
