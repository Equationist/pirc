-- Pass 4 Regression Test 2 (Stage P4-4):
--   - Cross-signedness conversion & membership (U8 <-> S8, Character'Val(200))
--   - Empty/null static range check
--   - 5D array indexing, bounds, descriptor passing
--   - Parenthesized array aggregate sliding
--   - Record rep-clause variant copy & rep-changing OUT/IN OUT view conversion
--   - Nested scalar DN_CONVERSION chain T2(T1(X)) on OUT/IN OUT
--   - Consolidated Unchecked_Conversion (float, unsigned, custom enum rep)
--   - Heap subcomponent 'CONSTRAINED = False (LRM 3.7.4(3))
--   - Nested record Emit_Default_Init preserving outer discriminant bindings
with TEXT_IO, UNCHECKED_CONVERSION;
use TEXT_IO;

procedure Pass4_Expr_Rep_Test is
   procedure Fail (Msg : String);

   type U8 is range 0 .. 255;
   for U8'Size use 8;

   type S8 is range -128 .. 127;
   for S8'Size use 8;

   subtype Empty_S8 is S8 range 10 .. 5;

   -- 5D array type
   type Arr5D is array (Integer range <>, Integer range <>, Integer range <>, Integer range <>, Integer range <>) of Integer;

   -- Parenthesized sliding array aggregate (base type must be unconstrained per LRM 4.3.2(6))
   type Unc_Int_Arr is array (Integer range <>) of Integer;
   subtype Arr1_3 is Unc_Int_Arr (1 .. 3);

   -- Record rep-changing view conversion (OUT reads discriminants!)
   type Base_VRec (Flag : Boolean := False) is record
      Head : Integer := 10;
      case Flag is
         when False =>
            Small_V : Integer := 20;
         when True =>
            Big_A : Integer := 30;
            Big_B : Integer := 40;
      end case;
   end record;

   type Packed_VRec is new Base_VRec;
   for Packed_VRec use record
      Flag    at 0 range 0 .. 7;
      Head    at 4 range 0 .. 31;
      Small_V at 8 range 0 .. 31;
      Big_A   at 8 range 0 .. 31;
      Big_B   at 12 range 0 .. 31;
   end record;

   -- Nested scalar DN_CONVERSION chain T2(T1(X))
   subtype Mid_Int is Integer range 1 .. 100;
   type Der_Int is new Integer;

   -- Unchecked_Conversion custom enum rep (including derived override!) & unsigned/float
   type Color is (Red, Green, Blue);
   for Color use (Red => 5, Green => 10, Blue => 20);
   for Color'Size use 8;

   type Der_Color is new Color;
   for Der_Color use (Red => 16, Green => 17, Blue => 18);
   for Der_Color'Size use 8;

   function Color_To_U8 is new UNCHECKED_CONVERSION (Color, U8);
   function U8_To_Color is new UNCHECKED_CONVERSION (U8, Color);
   function Der_Color_To_U8 is new UNCHECKED_CONVERSION (Der_Color, U8);
   function U8_To_Der_Color is new UNCHECKED_CONVERSION (U8, Der_Color);
   function Flt_To_I32 is new UNCHECKED_CONVERSION (Float, Integer);
   function I32_To_Flt is new UNCHECKED_CONVERSION (Integer, Float);

   -- Heap subcomponent 'CONSTRAINED = False (LRM 3.7.4(3))
   type Holder is record
      Inner : Base_VRec;
   end record;
   type Holder_Acc is access Holder;

   -- Nested records with same discriminant name D for Emit_Default_Init save/restore
   type Inner_D (D : Integer := 3) is record
      X : Integer := D + 10;
   end record;
   type Outer_D (D : Integer := 7) is record
      I : Inner_D;
      Y : Integer := D * 10;
   end record;

   U  : U8 := 200;
   S  : S8 := -1;
   CE : Boolean;
   A5 : Arr5D (1 .. 2, 1 .. 1, 1 .. 2, 1 .. 1, 10 .. 11);
   PA : Arr1_3 := ((10 .. 12 => 7));
   PR : Packed_VRec (Flag => True);
   MV : Integer := 95;
   HP : Holder_Acc := new Holder;
   OD : Outer_D;
   NF : Float := -1.5;
   FI : Integer;

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P4_2: " & Msg);
      raise Program_Error;
   end Fail;

   function Sum5D (A : Arr5D) return Integer is
      Tot : Integer := 0;
   begin
      if A'First (5) /= 10 or else A'Last (5) /= 11 or else A'Length (5) /= 2 then
         Fail ("5D bounds mismatch");
      end if;
      for I1 in A'Range (1) loop
         for I2 in A'Range (2) loop
            for I3 in A'Range (3) loop
               for I4 in A'Range (4) loop
                  for I5 in A'Range (5) loop
                     Tot := Tot + A (I1, I2, I3, I4, I5);
                  end loop;
               end loop;
            end loop;
         end loop;
      end loop;
      return Tot;
   end Sum5D;

   procedure Fill_Out_Base (R : out Base_VRec) is
   begin
      if R.Flag then
         R := (Flag => True, Head => 99, Big_A => 111, Big_B => 222);
      else
         R := (Flag => False, Head => 88, Small_V => 77);
      end if;
   end Fill_Out_Base;

   procedure Inc_Der (X : in out Der_Int) is
   begin
      X := X + 10;
   end Inc_Der;
begin
   -- 1. Cross-signedness conversion checks
   CE := False;
   begin
      S := S8 (U);
   exception
      when CONSTRAINT_ERROR => CE := True;
   end;
   if not CE then Fail ("S8(U8'(200)) should raise CE"); end if;

   CE := False;
   begin
      U := U8 (S);
   exception
      when CONSTRAINT_ERROR => CE := True;
   end;
   if not CE then Fail ("U8(S8'(-1)) should raise CE"); end if;

   if not (U in 150 .. 250) or else (U in 0 .. 100) then
      Fail ("U8 membership");
   end if;

   -- 2. Null range check
   CE := False;
   begin
      declare
         ES : Empty_S8 := 7;
      begin
         if ES = 7 then null; end if;
      end;
   exception
      when CONSTRAINT_ERROR => CE := True;
   end;
   if not CE then Fail ("Empty_S8 should raise CE"); end if;

   -- 3. 5D array
   for I1 in 1 .. 2 loop
      for I3 in 1 .. 2 loop
         for I5 in 10 .. 11 loop
            A5 (I1, 1, I3, 1, I5) := I1 + I3 + I5;
         end loop;
      end loop;
   end loop;
   if Sum5D (A5) /= 108 then
      Fail ("5D array sum");
   end if;

   -- 4. Parenthesized sliding aggregate
   if PA (1) /= 7 or else PA (2) /= 7 or else PA (3) /= 7 then
      Fail ("Parenthesized sliding aggregate");
   end if;

   -- 5. Rep-changing OUT record view conversion (discriminant readable on entry!)
   Fill_Out_Base (Base_VRec (PR));
   if not PR.Flag or else PR.Head /= 99 or else PR.Big_A /= 111 or else PR.Big_B /= 222 then
      Fail ("Rep-changing OUT record view conversion");
   end if;

   -- 6. Nested scalar DN_CONVERSION chain T2(T1(X)) checks intermediate Mid_Int on copy-out!
   CE := False;
   begin
      Inc_Der (Der_Int (Mid_Int (MV)));
   exception
      when CONSTRAINT_ERROR => CE := True;
   end;
   if not CE then Fail ("Nested DN_CONVERSION intermediate Mid_Int check"); end if;

   -- 7. Consolidated Unchecked_Conversion (including derived enum rep override)
   if Color_To_U8 (Green) /= 10 or else U8_To_Color (20) /= Blue then
      Fail ("Unchecked_Conversion enum rep");
   end if;
   if Der_Color_To_U8 (Green) /= 17 or else U8_To_Der_Color (18) /= Blue then
      Fail ("Unchecked_Conversion derived enum rep override");
   end if;
   FI := Flt_To_I32 (NF);
   if I32_To_Flt (FI) /= NF then
      Fail ("Unchecked_Conversion negative float round-trip");
   end if;

   -- 8. Heap subcomponent 'CONSTRAINED = False (LRM 3.7.4(3))
   if HP.Inner'Constrained then
      Fail ("Heap subcomponent 'CONSTRAINED should be False");
   end if;
   HP.Inner := (Flag => True, Head => 1, Big_A => 2, Big_B => 3);
   if not HP.Inner.Flag or else HP.Inner.Big_B /= 3 then
      Fail ("Heap subcomponent reassignment");
   end if;

   -- 9. Nested record Emit_Default_Init outer discriminant binding preserved
   if OD.D /= 7 or else OD.I.D /= 3 or else OD.I.X /= 13 or else OD.Y /= 70 then
      Fail ("Nested record default init outer discriminant clobbered");
   end if;

   Put_Line ("PASS4_EXPR_REP_OK");
end Pass4_Expr_Rep_Test;
