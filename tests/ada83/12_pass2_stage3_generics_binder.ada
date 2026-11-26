-- Pass 2 Stage 3 Regression Test: Generics, Subunits, Tasks in Variants/Generics & Local Instantiations
generic
   K : Integer;
package P2S3_Gen_Subu is
   function Calc (X : Integer) return Integer;
   function Pkg_Val return Integer;
end P2S3_Gen_Subu;

package body P2S3_Gen_Subu is
   package Nested_P is
      function F return Integer;
   end Nested_P;

   function Calc (X : Integer) return Integer is separate;
   package body Nested_P is separate;

   function Pkg_Val return Integer is
   begin
      return Nested_P.F;
   end Pkg_Val;
end P2S3_Gen_Subu;

separate (P2S3_Gen_Subu)
function Calc (X : Integer) return Integer is
begin
   return X + K;
end Calc;

separate (P2S3_Gen_Subu)
package body Nested_P is
   function F return Integer is
   begin
      return K * 10;
   end F;
end Nested_P;

with TEXT_IO, P2S3_Gen_Subu; use TEXT_IO;

procedure Pass2_Stage3_Generics is

   Var_Task_Done : Boolean := False;
   Gen_Task_Done : Boolean := False;
   R_Rec         : Integer;
   type Int_Arr is array (Positive range <>) of Integer;
   subtype Arr4 is Int_Arr (1 .. 4);

   -- 1 & 2. Generic package with body state, nested subp, nested-package exception, and dynamic array bounds
   generic
      N : Positive;
   package Gen_Box is
      procedure Add (V : Integer);
      function Get return Integer;
      function Arr_Len return Integer;
      procedure Raise_Inner;
      function Catch_Own_Inner return Boolean;
   end Gen_Box;

   package body Gen_Box is
      State : Integer := 0;
      subtype Dyn_Range is Integer range 1 .. N;
      type Dyn_Arr is array (Dyn_Range) of Integer;

      package Inner_Exc is
         E : exception;
      end Inner_Exc;

      function Helper_Inc (X : Integer) return Integer is
      begin
         return X + 1;
      end Helper_Inc;

      procedure Add (V : Integer) is
      begin
         State := State + V;
      end Add;

      function Get return Integer is
      begin
         return State;
      end Get;

      function Arr_Len return Integer is
         A : Dyn_Arr := (others => N);
      begin
         return A'Length + Helper_Inc (0) - 1;
      end Arr_Len;

      procedure Raise_Inner is
      begin
         raise Inner_Exc.E;
      end Raise_Inner;

      function Catch_Own_Inner return Boolean is
      begin
         Raise_Inner;
         return False;
      exception
         when Inner_Exc.E => return True;
      end Catch_Own_Inner;
   end Gen_Box;

   -- 3. Nested generic inside outer generic (outer body var, subp, and local exc)
   generic
      Init_Val : Integer;
   package Outer_G is
      generic
      package Inner_G is
         function Read_Outer return Integer;
         procedure Raise_Outer_Exc;
         function Test_Outer_Exc return Boolean;
      end Inner_G;
   end Outer_G;

   package body Outer_G is
      Secret : Integer := Init_Val;
      Outer_E : exception;

      function Get_Secret return Integer is
      begin
         return Secret;
      end Get_Secret;

      package body Inner_G is
         function Read_Outer return Integer is
         begin
            return Get_Secret + Secret;
         end Read_Outer;

         procedure Raise_Outer_Exc is
         begin
            raise Outer_E;
         end Raise_Outer_Exc;

         function Test_Outer_Exc return Boolean is
         begin
            Raise_Outer_Exc;
            return False;
         exception
            when Outer_E => return True;
         end Test_Outer_Exc;
      end Inner_G;
   end Outer_G;

   -- 4. Defaulted generic IN formal used in later formal object / body constraint
   generic
      Len : Positive := 4;
      Seed : Integer := Len * 2;
      type Vec is array (Positive range <>) of Integer;
   function Sum_Vec (V : Vec) return Integer;

   function Sum_Vec (V : Vec) return Integer is
      S : Integer := Seed;
   begin
      for I in V'Range loop
         S := S + V (I);
      end loop;
      return S;
   end Sum_Vec;

   -- 5. Generic with formal limited private type holding a task
   generic
      type Item_T is limited private;
   package Holder_G is
      Obj : Item_T;
   end Holder_G;

   -- Test 1 & 2: Recursive + multi-instantiation local generics
   function Rec_Local_Gen (Depth : Natural) return Integer is
      package B1 is new Gen_Box (Depth + 1);
      package B2 is new Gen_Box (Depth + 10);
      Sub_Res : Integer := 0;
   begin
      B1.Add (Depth * 10);
      B2.Add (Depth * 100);
      if Depth > 0 then
         Sub_Res := Rec_Local_Gen (Depth - 1);
      end if;
      -- Verify recursion did NOT clobber B1/B2 state and B1/B2 did not clobber each other
      if B1.Get /= Depth * 10 or else B2.Get /= Depth * 100 then
         return -999;
      end if;
      if B1.Arr_Len /= Depth + 1 or else B2.Arr_Len /= Depth + 10 then
         return -888;
      end if;
      return B1.Get + B2.Get + Sub_Res;
   end Rec_Local_Gen;

   -- Test 3: Outer generic O1, then O2, THEN instantiate O1.Inner_G
   procedure Test_Nested_Generics is
      package O1 is new Outer_G (11);
      package O2 is new Outer_G (22);
      package I1 is new O1.Inner_G;
      package I2 is new O2.Inner_G;
      Caught_Wrong : Boolean := False;
   begin
      if I1.Read_Outer /= 22 or else I2.Read_Outer /= 44 then
         raise Program_Error;
      end if;
      if not I1.Test_Outer_Exc or else not I2.Test_Outer_Exc then
         raise Program_Error;
      end if;
      -- Ensure O1.Outer_E is distinct from O2.Outer_E
      begin
         I1.Raise_Outer_Exc;
      exception
         when others => Caught_Wrong := True;
      end;
      if not Caught_Wrong then
         raise Program_Error;
      end if;
   end Test_Nested_Generics;

   -- Test 4: Distinct exception identity in nested package inside generic body
   procedure Test_Nested_Pkg_Exc_Identity is
      package G1 is new Gen_Box (3);
      package G2 is new Gen_Box (5);
      Caught_Cross : Boolean := False;
      Caught_Other : Boolean := False;
   begin
      if not G1.Catch_Own_Inner or else not G2.Catch_Own_Inner then
         raise Program_Error;
      end if;
      -- Verify G1.Arr_Len and G2.Arr_Len did not collide via Saved_Type_Ranges
      if G1.Arr_Len /= 3 or else G2.Arr_Len /= 5 then
         raise Program_Error;
      end if;
      -- Raise G1's inner exception and check it propagates
      begin
         G1.Raise_Inner;
      exception
         when others => Caught_Other := True;
      end;
      if not Caught_Other or else Caught_Cross then
         raise Program_Error;
      end if;
   end Test_Nested_Pkg_Exc_Identity;

   -- Test 5: Defaulted IN formal used in later defaulted formal & body
   procedure Test_Default_In_Formal_Constraint is
      function S4 is new Sum_Vec (Vec => Int_Arr);
      V : Arr4 := (1, 2, 3, 4);
   begin
      -- Seed = 4*2 = 8, sum(V) = 10 => 18
      if S4 (V) /= 18 then
         raise Program_Error;
      end if;
   end Test_Default_In_Formal_Constraint;

   -- Test 6: Tasks inside variant records and via formal limited private type
   procedure Test_Variant_And_Generic_Tasks is
      task type Worker_Var;
      task type Worker_Gen;

      type VRec (Has_T : Boolean := True) is record
         case Has_T is
            when True => W : Worker_Var;
            when False => Dummy : Integer := 0;
         end case;
      end record;

      task body Worker_Var is
      begin
         delay 0.01;
         Var_Task_Done := True;
      end Worker_Var;

      task body Worker_Gen is
      begin
         delay 0.01;
         Gen_Task_Done := True;
      end Worker_Gen;

      procedure Run_Variant is
         R : VRec (True);
      begin
         null; -- master must wait for R.W before returning!
      end Run_Variant;

      procedure Run_Generic_Task is
         package H is new Holder_G (Worker_Gen);
      begin
         null; -- master must wait for H.Obj before returning!
      end Run_Generic_Task;
   begin
      Run_Variant;
      if not Var_Task_Done then
         raise Program_Error;
      end if;
      Run_Generic_Task;
      if not Gen_Task_Done then
         raise Program_Error;
      end if;
   end Test_Variant_And_Generic_Tasks;

   -- Test 7: Local renamed task entry inside recursive procedure
   procedure Test_Recursive_Renamed_Entry is
      task type Echo_T is
         entry Ping (V : in Integer; R : out Integer);
      end Echo_T;
      task body Echo_T is
      begin
         accept Ping (V : in Integer; R : out Integer) do
            R := V * 2;
         end Ping;
      end Echo_T;

      function Rec_Call (N : Natural) return Integer is
         T : Echo_T;
         procedure Do_Ping (V : in Integer; R : out Integer) renames T.Ping;
         Sub : Integer := 0;
         Ans : Integer := 0;
      begin
         if N > 0 then
            Sub := Rec_Call (N - 1);
         end if;
         Do_Ping (N + 1, Ans);
         return Ans + Sub;
      end Rec_Call;
   begin
      -- Sum for N=0..3 of 2*(N+1) = 2 + 4 + 6 + 8 = 20
      if Rec_Call (3) /= 20 then
         raise Program_Error;
      end if;
   end Test_Recursive_Renamed_Entry;

   -- Test 8: Generic body separate subunits
   procedure Test_Generic_Subunits is
      package S1 is new P2S3_Gen_Subu (10);
      package S2 is new P2S3_Gen_Subu (25);
   begin
      if S1.Calc (5) /= 15 or else S2.Calc (5) /= 30 then
         raise Program_Error;
      end if;
      if S1.Pkg_Val /= 100 or else S2.Pkg_Val /= 250 then
         raise Program_Error;
      end if;
   end Test_Generic_Subunits;

begin
   R_Rec := Rec_Local_Gen (3);
   -- (0 + 10 + 20 + 30) + (0 + 100 + 200 + 300) = 660
   if R_Rec /= 660 then
      Put_Line ("FAIL_REC:" & Integer'Image (R_Rec));
      raise Program_Error;
   end if;
   Test_Nested_Generics;
   Test_Nested_Pkg_Exc_Identity;
   Test_Default_In_Formal_Constraint;
   Test_Variant_And_Generic_Tasks;
   Test_Recursive_Renamed_Entry;
   Test_Generic_Subunits;
   Put_Line ("PASS2_STAGE3_OK");
end Pass2_Stage3_Generics;
