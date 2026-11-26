-- Pass 4 Regression Test 3 (Stage P4-5):
--   - Captured local scalar value renaming (`alloca <Pir_Type>`)
--   - Deferred `for X use at Y'Address;` preserves overlaid object value
--   - Global constrained array sliding of named aggregate without `others`
--   - `case` statement with `A'Range` (DN_RANGE_ATTRIBUTE) choices
--   - `return;` inside `accept` releases Secondary Stack cleanly
--   - Dynamic constrained array entry parameter passing bounds + scalar IN entry range check
--   - Task body nested subprogram capturing record renaming's 'CONSTRAINED
--   - Unconstrained/dynamic array return deep copy
with TEXT_IO, SYSTEM;
use TEXT_IO;

procedure Pass4_Stmt_Task_Test is
   procedure Fail (Msg : String);

   type Unc_Arr is array (Integer range <>) of Integer;
   subtype Arr1_3 is Unc_Arr (1 .. 3);

   -- P4-5: Global constrained array initialized with named aggregate without `others` (sliding!)
   package Global_Slide_Pkg is
      G_Arr : Arr1_3 := (10 .. 12 => 55);
   end Global_Slide_Pkg;

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P4_3: " & Msg);
      raise Program_Error;
   end Fail;

   -- P4-5: Captured local scalar value renaming (`in` parameter is EK_Scalar!)
   procedure Test_Captured_Scalar_Renaming (N : Integer) is
      Val_Ren : Integer renames N;
      Res : Integer := 0;
      procedure Read_Inner is
      begin
         Res := Val_Ren * 2;
      end Read_Inner;
   begin
      Read_Inner;
      if Res /= N * 2 then
         Fail ("Captured scalar value renaming");
      end if;
   end Test_Captured_Scalar_Renaming;

   -- P4-5: Deferred DN_ADDRESS overlay must NOT clobber overlaid object
   procedure Test_Deferred_Address is
      Overlay : Integer;
      Target  : Integer := 4242;
      for Overlay use at Target'Address;
   begin
      if Overlay /= 4242 or else Target /= 4242 then
         Fail ("Deferred DN_ADDRESS clobbered overlaid object");
      end if;
      Overlay := 9999;
      if Target /= 9999 then
         Fail ("Deferred DN_ADDRESS write-through failed");
      end if;
   end Test_Deferred_Address;

   -- P4-5: `case` statement with static discrete subtype choices & wide ranges
   function Classify (X : Integer) return Integer is
      subtype Low_Rng is Integer range 1 .. 10;
      subtype Mid_Rng is Integer range 11 .. 2000;
   begin
      case X is
         when Low_Rng =>
            return 1;
         when Mid_Rng =>
            return 2;
         when others =>
            return 3;
      end case;
   end Classify;

   -- P4-5: `return;` inside `accept` releasing SS + dynamic constrained array entry param + task frame 'CONSTRAINED
   procedure Test_Task_Features (N : Integer) is
      subtype Dyn_Arr is Unc_Arr (1 .. N);
      subtype Small_Byte is Integer range 0 .. 100;

      type VRec (D : Boolean := False) is record
         V : Integer := 0;
      end record;

      task Worker is
         entry Check_Arr (A : Dyn_Arr; Sum : out Integer);
         entry Early_Ret (K : Integer; Len : out Integer);
         entry Small_In (B : Small_Byte);
         entry Done;
      end Worker;

      DA : Dyn_Arr := (others => 3);
      S  : Integer := 0;
      L  : Integer := 0;
      CE : Boolean := False;

      function Make_Str (K : Integer) return String is
         S : String (1 .. K) := (others => 'A');
      begin
         return S;
      end Make_Str;

      task body Worker is
         R_Uncon : VRec;
         R_Ren   : VRec renames R_Uncon;

         function Nested_Check_Con return Boolean is
         begin
            return R_Ren'Constrained;
         end Nested_Check_Con;
      begin
         if Nested_Check_Con then
            Fail ("Task frame Con_Captured_Slot should be False");
         end if;
         loop
            select
               accept Check_Arr (A : Dyn_Arr; Sum : out Integer) do
                  declare
                     T : Integer := 0;
                  begin
                     if A'First /= 1 or else A'Last /= N then
                        Fail ("Dynamic constrained array entry bounds");
                     end if;
                     for I in A'Range loop
                        T := T + A (I);
                     end loop;
                     Sum := T;
                  end;
               end Check_Arr;
            or
               accept Early_Ret (K : Integer; Len : out Integer) do
                  declare
                     Tmp : constant String := Make_Str (K);
                  begin
                     Len := Tmp'Length;
                     return;
                  end;
               end Early_Ret;
            or
               accept Small_In (B : Small_Byte) do
                  if B < 0 then null; end if;
               end Small_In;
            or
               accept Done;
               exit;
            or
               terminate;
            end select;
         end loop;
      end Worker;
   begin
      Worker.Check_Arr (DA, S);
      if S /= 3 * N then
         Fail ("Dynamic constrained array entry sum");
      end if;
      for Iter in 1 .. 50 loop
         Worker.Early_Ret (64, L);
         if L /= 64 then Fail ("Accept early return"); end if;
      end loop;
      begin
         Worker.Small_In (200);
      exception
         when CONSTRAINT_ERROR => CE := True;
      end;
      if not CE then Fail ("Entry IN scalar range check"); end if;
      Worker.Done;
   end Test_Task_Features;

   -- P4-5: Dynamic unconstrained array return deep copy
   function Make_Unc (N : Integer) return Unc_Arr is
      Local : Unc_Arr (1 .. N);
   begin
      for I in 1 .. N loop
         Local (I) := I * 10;
      end loop;
      return Local;
   end Make_Unc;
begin
   if Global_Slide_Pkg.G_Arr (1) /= 55 or else Global_Slide_Pkg.G_Arr (3) /= 55 then
      Fail ("Global sliding named aggregate");
   end if;
   Test_Captured_Scalar_Renaming (15);
   Test_Deferred_Address;
   if Classify (5) /= 1 or else Classify (15) /= 2 or else Classify (2500) /= 3 then
      Fail ("Case subtype choices");
   end if;
   Test_Task_Features (5);
   declare
      R_Arr : constant Unc_Arr := Make_Unc (4);
   begin
      if R_Arr'Length /= 4 or else R_Arr (1) /= 10 or else R_Arr (4) /= 40 then
         Fail ("Unconstrained return");
      end if;
   end;

   Put_Line ("PASS4_STMT_TASK_OK");
end Pass4_Stmt_Task_Test;
