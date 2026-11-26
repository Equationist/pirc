-- Pass 3 Stage 5 Regression Test (also covers Pass 1 & Pass 2 generics/exceptions/subunits):
--   P3-5.1:  Local instantiation of nested generic (O1.Inner) capturing outer variables
--   P3-5.2:  Multiple instantiations of generic package containing a task type
--   P3-5.3:  Generic package with separate subunit instantiated twice (distinct state, no dup sym)
--   P3-5.4:  Two generic package instantiations inside a task body have isolated state
--   P3-5.5:  Generic package body with DN_RENAMES_OBJ_DECL and variant record components
--   P3-5.6:  Constrained subtype of generic formal discriminated private type (=, in, discr check)
--   P3-5.7:  Local TEXT_IO.INTEGER_IO in recursive proc + user generic shadowing INTEGER_IO + task entry actual
--   P3-5.8:  Generic formal T'Image/'Value and array index type substitution
--   P3-5.9:  Nested generic package instantiation inside generic body: spec exception distinct & caught
--   P3-5.11: Unelaborated generic subprogram call caught in local handler (raises Program_Error)
with TEXT_IO;
use TEXT_IO;

generic
   Init_Val : Integer;
package P3S5_Gen_Sub is
   function Compute (X : Integer) return Integer;
end P3S5_Gen_Sub;

package body P3S5_Gen_Sub is
   State : Integer := Init_Val;
   function Helper (Y : Integer) return Integer is separate;
   function Compute (X : Integer) return Integer is
   begin
      return Helper (X);
   end Compute;
end P3S5_Gen_Sub;

separate (P3S5_Gen_Sub)
function Helper (Y : Integer) return Integer is
begin
   State := State + Y;
   return State;
end Helper;

with TEXT_IO, P3S5_Gen_Sub;
use TEXT_IO;

procedure Pass3_Stage5_Test is

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P3S5: " & Msg);
      raise Program_Error;
   end Fail;

   -- P3-5.1 & P3-5.9: Nested generic with outer capture & inner spec exception
   generic
      Base_K : Integer;
   package Outer_Gen is
      function Run_Inner (V : Integer) return Integer;
      function Test_Inner_Exc (Trigger : Boolean) return Integer;
   end Outer_Gen;

   package body Outer_Gen is
      Outer_State : Integer := Base_K;

      generic
         Mult : Integer;
      package Inner_Gen is
         Inner_Err : exception;
         function Eval (X : Integer) return Integer;
         procedure Boom;
      end Inner_Gen;

      package body Inner_Gen is
         function Eval (X : Integer) return Integer is
         begin
            Outer_State := Outer_State + X * Mult;
            return Outer_State;
         end Eval;
         procedure Boom is
         begin
            raise Inner_Err;
         end Boom;
      end Inner_Gen;

      package IG_Body is new Inner_Gen (2);

      function Run_Inner (V : Integer) return Integer is
         package IG_Loc is new Inner_Gen (3);
      begin
         return IG_Loc.Eval (V);
      end Run_Inner;

      function Test_Inner_Exc (Trigger : Boolean) return Integer is
      begin
         if Trigger then
            IG_Body.Boom;
         end if;
         return 0;
      exception
         when IG_Body.Inner_Err =>
            return Base_K;
      end Test_Inner_Exc;
   end Outer_Gen;

   -- P3-5.2: Generic package containing a task type
   generic
      Bias : Integer;
   package Task_Gen is
      task type T is
         entry Ask (V : Integer; R : out Integer);
      end T;
   end Task_Gen;

   package body Task_Gen is
      task body T is
      begin
         accept Ask (V : Integer; R : out Integer) do
            R := V + Bias;
         end Ask;
      end T;
   end Task_Gen;

   -- P3-5.4 & P3-5.5: Generic with state, object renaming & variant record
   generic
      Seed : Integer;
   package State_Gen is
      function Step return Integer;
   end State_Gen;

   package body State_Gen is
      type VR (Flag : Boolean := True) is record
         case Flag is
            when True  => A : Integer := Seed;
            when False => B : Integer := -Seed;
         end case;
      end record;
      Counter : Integer := Seed;
      C_Ren   : Integer renames Counter;
      Rec     : VR (True);

      function Step return Integer is
      begin
         C_Ren := C_Ren + 1;
         return C_Ren + Rec.A;
      end Step;
   end State_Gen;

   -- P3-5.6: Formal discriminated private type
   generic
      type Priv (D : Integer) is private;
      with function Make (N : Integer) return Priv;
   package Discr_Gen is
      function Check_Sub (X : Priv) return Boolean;
   end Discr_Gen;

   package body Discr_Gen is
      subtype P2 is Priv (2);
      function Check_Sub (X : Priv) return Boolean is
         Y : P2 := Make (2);
      begin
         return (X in P2) and then (X = Y);
      end Check_Sub;
   end Discr_Gen;

   -- P3-5.7: User generic shadowing RTL name INTEGER_IO
   generic
      type T is (<>);
   package INTEGER_IO is
      function First_Pos return Integer;
   end INTEGER_IO;

   package body INTEGER_IO is
      function First_Pos return Integer is
      begin
         return T'Pos (T'First);
      end First_Pos;
   end INTEGER_IO;

   -- P3-5.8: Formal discrete T'Image/'Value and array index substitution
   generic
      type Idx is (<>);
   package Idx_Gen is
      type Map_T is array (Idx) of Integer;
      function Roundtrip (V : Idx) return Boolean;
   end Idx_Gen;

   package body Idx_Gen is
      function Roundtrip (V : Idx) return Boolean is
         M : Map_T := (others => 1);
      begin
         return M (Idx'Value (Idx'Image (V))) = 1;
      end Roundtrip;
   end Idx_Gen;

   procedure Test_Nested_And_Subunit is
      package O1 is new Outer_Gen (10);
      package O2 is new Outer_Gen (100);
      package S1 is new P3S5_Gen_Sub (5);
      package S2 is new P3S5_Gen_Sub (50);
   begin
      if O1.Run_Inner (2) /= 16 or else O2.Run_Inner (2) /= 106 then
         Fail ("Nested generic outer capture");
      end if;
      if O1.Test_Inner_Exc (True) /= 10 or else O2.Test_Inner_Exc (True) /= 100 then
         Fail ("Nested generic spec exception");
      end if;
      if S1.Compute (3) /= 8 or else S2.Compute (3) /= 53 or else S1.Compute (2) /= 10 then
         Fail ("Generic subunit isolation");
      end if;
   end Test_Nested_And_Subunit;

   procedure Test_Generic_Tasks is
      package TG1 is new Task_Gen (10);
      package TG2 is new Task_Gen (200);
      T1 : TG1.T;
      T2 : TG2.T;
      R1, R2 : Integer;
   begin
      T1.Ask (5, R1);
      T2.Ask (5, R2);
      if R1 /= 15 or else R2 /= 205 then
         Fail ("Generic task Body_Fn clobber");
      end if;

      -- P3-5.4: Two generic instantiations inside a task body
      declare
         task Isol_T is
            entry Check (Ok : out Boolean);
         end Isol_T;
         Ok : Boolean;
         task body Isol_T is
            package G1 is new State_Gen (10);
            package G2 is new State_Gen (100);
            V1, V2 : Integer;
         begin
            V1 := G1.Step;
            V2 := G2.Step;
            accept Check (Ok : out Boolean) do
               Ok := (V1 = 21) and then (V2 = 201);
            end Check;
         end Isol_T;
      begin
         Isol_T.Check (Ok);
         if not Ok then
            Fail ("Task body generic instantiation isolation");
         end if;
      end;
   end Test_Generic_Tasks;

   procedure Test_Formal_Discr_And_Shadow_And_RecIO is
      type My_Rec (D : Integer := 1) is record
         V : Integer := 0;
      end record;
      function Mk (N : Integer) return My_Rec;
      package DG is new Discr_Gen (My_Rec, Mk);

      type Color is (Red, Green, Blue);
      package Shadow_IO is new INTEGER_IO (Color);
      package Color_Idx is new Idx_Gen (Color);

      generic
         with procedure Ping (V : out Integer);
      function Call_Ping return Integer;

      task Local_Srv is
         entry Get_V (V : out Integer);
      end Local_Srv;

      function Mk (N : Integer) return My_Rec is
      begin
         return (D => N, V => 42);
      end Mk;

      -- P3-5.7: local TEXT_IO.INTEGER_IO in recursive proc
      procedure Rec_IO (N : Integer) is
         package L_IO is new TEXT_IO.INTEGER_IO (Integer);
         S : String (1 .. 10);
         V : Integer;
         L : Positive;
      begin
         L_IO.Default_Width := N + 2;
         if N > 1 then
            Rec_IO (N - 1);
         end if;
         if L_IO.Default_Width /= N + 2 then
            Fail ("Recursive local TEXT_IO.INTEGER_IO Default_Width clobbered");
         end if;
         L_IO.Put (S, N * 11);
         L_IO.Get (S, V, L);
         if V /= N * 11 then
            Fail ("Recursive local TEXT_IO.INTEGER_IO roundtrip");
         end if;
      end Rec_IO;

      function Call_Ping return Integer is
         R : Integer;
      begin
         Ping (R);
         return R;
      end Call_Ping;

      task body Local_Srv is
      begin
         accept Get_V (V : out Integer) do V := 77; end Get_V;
      end Local_Srv;

      function Do_Ping is new Call_Ping (Local_Srv.Get_V);
   begin
      if not DG.Check_Sub (Mk (2)) or else DG.Check_Sub (Mk (3)) then
         Fail ("Formal discriminated private subtype check");
      end if;
      if Shadow_IO.First_Pos /= 0 then
         Fail ("User generic shadowing INTEGER_IO");
      end if;
      if not Color_Idx.Roundtrip (Green) then
         Fail ("Formal discrete Image/Value/array idx");
      end if;
      Rec_IO (3);
      if Do_Ping /= 77 then
         Fail ("Local task entry generic actual");
      end if;
   end Test_Formal_Discr_And_Shadow_And_RecIO;

   -- P3-5.11: Unelaborated generic subprogram caught in handler
   procedure Test_Unelab_Gen_Subp is
      Caught : Boolean := False;
   begin
      begin
         declare
            generic
            function Late_F return Integer;
            function Inst_F is new Late_F;
            X : Integer;

            function Late_F return Integer is
            begin
               return 1;
            end Late_F;
         begin
            X := Inst_F;
            if X = 1 then null; end if;
         end;
      exception
         when Program_Error =>
            Caught := True;
      end;
      if not Caught then
         Fail ("Unelab generic subp did not raise Program_Error");
      end if;
   end Test_Unelab_Gen_Subp;

begin
   Test_Nested_And_Subunit;
   Test_Generic_Tasks;
   Test_Formal_Discr_And_Shadow_And_RecIO;
   Test_Unelab_Gen_Subp;
   Put_Line ("PASS3_STAGE5_OK");
end Pass3_Stage5_Test;
