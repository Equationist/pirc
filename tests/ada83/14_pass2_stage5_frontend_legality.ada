-- Stage P2-5 regression test: Ada 83 frontend legality, visibility, renaming,
-- overloaded defaults, bare raise in accept inside handler, entry-family renaming,
-- DURATION'Safe_Large, and pragmas.
with TEXT_IO; use TEXT_IO;

procedure Main_P2S5 is
   procedure Fail (Msg : String) is
   begin
      PUT_LINE ("FAIL: " & Msg);
      raise PROGRAM_ERROR;
   end Fail;

   -- 1. DURATION'Safe_Large <= DURATION'Base'Last
   procedure Test_Duration_Attrs is
   begin
      if DURATION'SAFE_LARGE > DURATION'BASE'LAST then
         Fail ("DURATION'Safe_Large > DURATION'Base'Last");
      end if;
      if -DURATION'SAFE_LARGE < DURATION'BASE'FIRST then
         Fail ("-DURATION'Safe_Large < DURATION'Base'First");
      end if;
   end Test_Duration_Attrs;

   -- 2. Overloaded function with required parameters does not cause false ambiguity
   --    in parameterless expression context (GET_DEF_EXP_TYPE).
   procedure Test_Overload_Visibility is
      function G return Integer is
      begin
         return 7;
      end G;

      function G (X : Integer) return Float is
      begin
         return Float (X) + 0.5;
      end G;
   begin
      declare
         B : Integer := G;
         C : Float := G (4);
      begin
         if B /= 7 then
            Fail ("Parameterless G wrong");
         end if;
         if C < 4.4 or else C > 4.6 then
            Fail ("Overloaded G(4) wrong");
         end if;
      end;
   end Test_Overload_Visibility;

   -- 3. use Renamed_Pkg in package spec remains visible in package body
   procedure Test_Use_Renamed_Pkg is
      package Orig is
         Magic : constant Integer := 123;
      end Orig;

      package Alias renames Orig;

      package Client is
         use Alias;
         function Get_Magic return Integer;
      end Client;

      package body Client is
         function Get_Magic return Integer is
         begin
            return Magic;
         end Get_Magic;
      end Client;
   begin
      if Client.Get_Magic /= 123 then
         Fail ("use Renamed_Pkg in body failed");
      end if;
   end Test_Use_Renamed_Pkg;

   -- 4. Bare raise; inside accept enclosed by an exception handler (LRM 11.3(3))
   procedure Test_Bare_Raise_In_Accept is
      Custom_Err : exception;
      Caller_Caught : Boolean := False;
      Task_Caught : Boolean := False;

      task T is
         entry Ping;
      end T;

      task body T is
      begin
         begin
            raise Custom_Err;
         exception
            when Custom_Err =>
               Task_Caught := True;
               accept Ping do
                  raise;
               end Ping;
         end;
      exception
         when Custom_Err =>
            null;
      end T;
   begin
      begin
         T.Ping;
      exception
         when Custom_Err =>
            Caller_Caught := True;
      end;
      if not (Task_Caught and Caller_Caught) then
         Fail ("Bare raise in accept inside handler failed");
      end if;
   end Test_Bare_Raise_In_Accept;

   -- 5. Renaming an overloaded entry family member + pragma SUPPRESS on overloaded subp
   procedure Test_Entry_Family_Rename_And_Suppress is
      Seen : Integer := 0;

      task Worker is
         entry Fam (1 .. 3) (V : Integer);
         entry Other (V : Boolean);
      end Worker;

      procedure Call_Fam2 (V : Integer) renames Worker.Fam (2);

      procedure Ov (X : Integer) is
      begin
         Seen := Seen + X;
      end Ov;

      procedure Ov (X : Boolean) is
      begin
         if X then
            Seen := Seen + 100;
         end if;
      end Ov;

      pragma SUPPRESS (RANGE_CHECK, ON => Ov);

      task body Worker is
      begin
         accept Fam (2) (V : Integer) do
            Seen := Seen + V;
         end Fam;
      end Worker;
   begin
      Ov (5);
      Ov (True);
      Call_Fam2 (17);
      if Seen /= 122 then
         Fail ("Entry family rename or pragma SUPPRESS failed");
      end if;
   end Test_Entry_Family_Rename_And_Suppress;

   -- 6. Object renaming: non-discriminant dynamic array component is legal,
   --    and dereferenced component of constant access object is a variable.
   procedure Test_Object_Renaming_Cases is
      N : Integer := 4;
      type Rec (D : Integer := 1) is record
         Fixed_Arr : String (1 .. N);
      end record;
      type Rec_Ptr is access Rec;

      R : Rec;
      S_Ren : String renames R.Fixed_Arr;

      C_Ptr : constant Rec_Ptr := new Rec'(D => 1, Fixed_Arr => "abcd");
      V_Ren : String renames C_Ptr.Fixed_Arr;
   begin
      S_Ren := "WXYZ";
      if R.Fixed_Arr /= "WXYZ" then
         Fail ("Non-discriminant dynamic array component renaming failed");
      end if;
      V_Ren (1) := 'Q';
      if C_Ptr.Fixed_Arr /= "Qbcd" then
         Fail ("Constant access deref component renaming failed");
      end if;
   end Test_Object_Renaming_Cases;

begin
   Test_Duration_Attrs;
   Test_Overload_Visibility;
   Test_Use_Renamed_Pkg;
   Test_Bare_Raise_In_Accept;
   Test_Entry_Family_Rename_And_Suppress;
   Test_Object_Renaming_Cases;
   PUT_LINE ("PASS2_STAGE5_OK");
end Main_P2S5;
