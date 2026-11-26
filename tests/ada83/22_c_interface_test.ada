-- Pass 3 Stage 6 Regression Test: pragma INTERFACE (C, ...) (P3-6.9)
with TEXT_IO;
use TEXT_IO;

procedure C_Interface_Test is
   function P3_C_Add (A, B : Integer) return Integer;
   pragma INTERFACE (C, P3_C_Add);

   function P3_C_Mul_F64 (A, B : Long_Float) return Long_Float;
   pragma INTERFACE (C, P3_C_Mul_F64);

   procedure P3_C_Bump (X : in out Integer; Delta_V : Integer);
   pragma INTERFACE (C, P3_C_Bump);

   function P3_C_Sum_Bytes (S : String; N : Integer) return Integer;
   pragma INTERFACE (C, P3_C_Sum_Bytes);

   Val : Integer := 10;
   S   : constant String := "ABC";
begin
   if P3_C_Add (19, 23) /= 42 then
      raise Program_Error;
   end if;
   if P3_C_Mul_F64 (2.5, 4.0) /= 10.0 then
      raise Program_Error;
   end if;
   P3_C_Bump (Val, 7);
   if Val /= 17 then
      raise Program_Error;
   end if;
   -- 'A'=65, 'B'=66, 'C'=67 -> 198
   if P3_C_Sum_Bytes (S, S'Length) /= 198 then
      raise Program_Error;
   end if;
   Put_Line ("C_INTERFACE_OK");
end C_Interface_Test;
