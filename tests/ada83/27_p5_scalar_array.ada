-- Pass-5 regression test: R1 (i64 discrete semantics & conversions) and R2 (64-bit array math & aggregates)
with Text_IO; use Text_IO;

procedure P5_Scalar_Array is
   Failed : Integer := 0;
   procedure Check (Cond : Boolean; Label : String);

   -- R1: 8-bit & 16-bit enums / unsigned discrete checks
   type Byte_Range is range 0 .. 255;
   for Byte_Range'Size use 8;
   type Word_Range is range 0 .. 65535;
   for Word_Range'Size use 16;

   type Small_Enum is (E0, E1, E2, E3, E4);
   subtype Mid_Enum is Small_Enum range E1 .. E3;

   -- R1: Custom character enum for 2D string subaggregate
   type Hex_Char is ('0', '1', 'A', 'B');
   type Hex_Mat is array (Positive range <>, Positive range <>) of Hex_Char;

   -- R1: Fixed-point exact rational ticks
   type Fx is delta 0.125 range -16.0 .. 16.0;

   -- R2: Large 64-bit Long_Integer array bounds
   Base_Idx : constant Long_Integer := 10_000_000_000;
   type Big_Idx is new Long_Integer;
   type Big_Arr is array (Big_Idx range <>) of Integer;
   type Big_Mat is array (Big_Idx range <>, Big_Idx range <>) of Integer;

   B : Byte_Range := 250;
   W : Word_Range := 60000;
   F : Fx := 2.25;
   SF : Float := 1.5;
   LF : Long_Float := 2.5;
   Caught : Boolean;

   procedure Check (Cond : Boolean; Label : String) is
   begin
      if not Cond then
         Put_Line ("FAIL: " & Label);
         Failed := Failed + 1;
      end if;
   end Check;

   function Make_Big (Lo, Hi : Big_Idx) return Big_Arr is
      R : Big_Arr (Lo .. Hi);
   begin
      for I in R'Range loop
         R (I) := Integer (I - Lo) + 10;
      end loop;
      return R;
   end Make_Big;

   function Id_Int (X : Integer) return Integer is
   begin
      return X;
   end Id_Int;

   function Id_Big (X : Big_Idx) return Big_Idx is
   begin
      return X;
   end Id_Big;
begin
   -- 1. Unsigned 8-bit/16-bit arithmetic, attributes, and overflow checks
   Check (Byte_Range'Pos (B) = 250, "byte_pos");
   Check (Byte_Range'Pred (B) = 249, "byte_pred");
   Check (Byte_Range'Succ (B) = 251, "byte_succ");
   Check (Word_Range'Val (60000) = W, "word_val");
   Check (Small_Enum'Image (E3) = "E3", "enum_image");
   Check (Mid_Enum'Value ("E2") = E2, "enum_value");

   Caught := False;
   begin
      B := Byte_Range'Succ (Byte_Range'Last);
   exception
      when Constraint_Error => Caught := True;
   end;
   Check (Caught, "byte_succ_ovf");

   Caught := False;
   begin
      declare
         M : Mid_Enum := Mid_Enum'Value ("E4");
      begin
         Check (M = E4, "unreachable");
      end;
   exception
      when Constraint_Error => Caught := True;
   end;
   Check (Caught, "mid_enum_value_ce");

   -- 2. Fixed-point & Short_Float conversions
   F := F + Fx (0.5);
   Check (F = 2.75, "fixed_add");
   Check (Integer (F) = 3, "fixed_to_int_round");
   Check (Integer (Fx (-2.5)) = -3 or else Integer (Fx (-2.5)) = -2, "fixed_neg_round");
   SF := SF + Float (LF);
   Check (SF > 3.9 and then SF < 4.1, "short_float_add");

   -- 3. Large 64-bit array bounds (Big_Idx > 2**32)
   declare
      Lo : constant Big_Idx := Id_Big (Big_Idx (Base_Idx));
      Hi : constant Big_Idx := Id_Big (Big_Idx (Base_Idx + 4));
      A  : constant Big_Arr := Make_Big (Lo, Hi);
   begin
      Check (A'First = Lo, "big_first");
      Check (A'Last = Hi, "big_last");
      Check (A'Length = 5, "big_length");
      Check (A (Lo) = 10 and then A (Hi) = 14, "big_elems");
   end;

   -- 4. Super-null 2D array with huge second dimension (must NOT raise Storage_Error!)
   declare
      Empty_M : Big_Mat (1 .. Id_Big (0), 1 .. Id_Big (Big_Idx'Last));
   begin
      Check (Empty_M'Length (1) = 0, "super_null_len1");
      Check (Empty_M'Length (2) > 0, "super_null_len2");
   end;

   -- 5. Named array aggregate without others (sliding) + 2D custom-char string subaggregate
   declare
      type Unc_Arr is array (Integer range <>) of Integer;
      subtype Arr3 is Unc_Arr (1 .. 3);
      X : Arr3 := (10 => 100, 11 => 200, 12 => 300);
      M : Hex_Mat (1 .. 2, 1 .. 3) := ("01A", "BA0");
   begin
      Check (X (1) = 100 and then X (2) = 200 and then X (3) = 300, "named_slide");
      Check (M (1, 1) = '0' and then M (1, 3) = 'A' and then M (2, 1) = 'B', "hex_mat");
   end;

   -- 6. Out-of-bounds multi-D string subaggregate check on unconstrained aggregate
   declare
      subtype Sub2 is Positive range 1 .. Id_Int (2);
      type S2 is array (Positive range <>, Sub2 range <>) of Hex_Char;
      V : S2 (1 .. 2, 1 .. 2) := ("01", "AB");
   begin
      Check (V (2, 2) = 'B', "hex_sub_ok");
   end;

   if Failed = 0 then
      Put_Line ("P5_SCALAR_ARRAY_OK");
   else
      Put_Line ("P5_SCALAR_ARRAY_FAILED");
   end if;
end P5_Scalar_Array;
