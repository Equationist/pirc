-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Characters.Handling;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces; use Interfaces;

package body Ada_Sym_Util is

   function Cap_Pir_Sym (Full : String; Suffix : String := "") return String is
   begin
      if Full'Length <= 96 then
         return Full;
      end if;
      declare
         H    : Unsigned_64 := 16#CBF2_9CE4_8422_2325#;
         Hex  : constant String := "0123456789abcdef";
         HStr : String (1 .. 16);
         Tmp  : Unsigned_64;
      begin
         for C of Full loop
            H := (H xor Character'Pos (C)) * 16#0000_0100_0000_01B3#;
         end loop;
         Tmp := H;
         for K in reverse 1 .. 16 loop
            HStr (K) := Hex (Natural (Tmp and 15) + 1);
            Tmp := Shift_Right (Tmp, 4);
         end loop;
         return Full (Full'First .. Full'First + 47) & "_h" & HStr & Suffix;
      end;
   end Cap_Pir_Sym;

   function Sanitize_Ada_Ident (S : String) return String is
      R : Unbounded_String;
   begin
      for C of S loop
         if C in 'a' .. 'z' | 'A' .. 'Z' | '0' .. '9' | '_' then
            Append (R, Ada.Characters.Handling.To_Lower (C));
         elsif C = '+' then Append (R, "op_plus");
         elsif C = '-' then Append (R, "op_minus");
         elsif C = '*' then Append (R, "op_mul");
         elsif C = '/' then Append (R, "op_div");
         elsif C = '=' then Append (R, "op_eq");
         elsif C = '<' then Append (R, "op_lt");
         elsif C = '>' then Append (R, "op_gt");
         elsif C = '&' then Append (R, "op_amp");
         elsif C /= '"' then
            Append (R, '_');
         end if;
      end loop;
      if Length (R) = 0 then
         return "anon";
      end if;
      return To_String (R);
   end Sanitize_Ada_Ident;

   function Main_Entry_Sym (Unit_Name : String) return String is
   begin
      return Cap_Pir_Sym ("@ada__" & Sanitize_Ada_Ident (Unit_Name));
   end Main_Entry_Sym;

end Ada_Sym_Util;
