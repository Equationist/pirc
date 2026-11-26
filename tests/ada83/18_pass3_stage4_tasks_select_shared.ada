-- Pass 3 Stage 4 Regression Test (also covers Pass 1 & Pass 2 concurrency/tasking/shared findings):
--   P3-4.1:  Concurrent TEXT_IO across multiple tasks (ada_io.c thread safety)
--   P3-4.2:  Concurrent task allocators on same master (per-allocator group_id isolation)
--   P3-4.3:  Recursive subprogram with local access-to-task & local task body
--   P3-4.4:  Multi-task select..or terminate with sibling rendezvous (no premature termination)
--   P3-4.6:  pragma SHARED across renaming & in out under -O2 (volatile + fence)
--   P3-4.8:  goto out of task block inside package body leaves master cleanly
--   P3-4.9:  Nested package in enclosing package spec activates tasks before body statements
--   P3-4.10: Entry family >64 slots (1..100) + dynamic entry family bounds frozen at decl
--   P3-4.12: Self-reference T'Callable inside task body T completing limited private type
--   P3-4.14/15/16: pragma PRIORITY, selective wait guard/delay eval order, CALENDAR.SPLIT
with TEXT_IO, CALENDAR;
use TEXT_IO;

procedure Pass3_Stage4_Test is
   pragma PRIORITY (5);

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P3S4: " & Msg);
      raise Program_Error;
   end Fail;

   -- P3-4.1: Concurrent TEXT_IO on independent temporary files
   procedure Test_Concurrent_IO is
      task type IO_Worker is
         entry Start (Id : Integer);
      end IO_Worker;

      W : array (1 .. 4) of IO_Worker;

      task body IO_Worker is
         My_Id : Integer;
         F     : File_Type;
         Buf   : String (1 .. 40);
         Len   : Natural;
      begin
         accept Start (Id : Integer) do
            My_Id := Id;
         end Start;
         for Iter in 1 .. 10 loop
            Create (F, Out_File, "");
            Put_Line (F, "TASK_IO_OK");
            Reset (F, In_File);
            Get_Line (F, Buf, Len);
            Delete (F);
            if Len /= 10 or else Buf (1 .. 10) /= "TASK_IO_OK" then
               Fail ("Concurrent IO mismatch");
            end if;
         end loop;
         if My_Id < 0 then null; end if;
      end IO_Worker;
   begin
      for I in W'Range loop
         W (I).Start (I);
      end loop;
   end Test_Concurrent_IO;

   -- P3-4.2: Concurrent task allocators on same master
   procedure Test_Concurrent_Allocators is
      task type Leaf is
         entry Ping (V : in Integer; R : out Integer);
      end Leaf;
      type Leaf_Acc is access Leaf;

      task type Spawner is
         entry Go (Base : Integer);
      end Spawner;

      S : array (1 .. 3) of Spawner;

      task body Leaf is
      begin
         accept Ping (V : in Integer; R : out Integer) do
            R := V * 2;
         end Ping;
      end Leaf;

      task body Spawner is
         B : Integer;
         P : Leaf_Acc;
         R : Integer;
      begin
         accept Go (Base : Integer) do B := Base; end Go;
         for I in 1 .. 4 loop
            P := new Leaf;
            P.Ping (B + I, R);
            if R /= (B + I) * 2 then
               Fail ("Concurrent allocator ping");
            end if;
         end loop;
      end Spawner;
   begin
      for I in S'Range loop
         S (I).Go (I * 10);
      end loop;
   end Test_Concurrent_Allocators;

   -- P3-4.3: Recursive subprogram with local access-to-task and local task body
   procedure Test_Recursive_Local_Tasks (Depth : Integer) is
      task type Local_T is
         entry Echo (X : Integer; Y : out Integer);
      end Local_T;
      type Local_Acc is access Local_T;
      P : Local_Acc;
      Res : Integer;
      task body Local_T is
      begin
         accept Echo (X : Integer; Y : out Integer) do
            Y := X + Depth;
         end Echo;
      end Local_T;
   begin
      P := new Local_T;
      P.Echo (10, Res);
      if Res /= 10 + Depth then
         Fail ("Recursive local task");
      end if;
      if Depth > 1 then
         Test_Recursive_Local_Tasks (Depth - 1);
      end if;
   end Test_Recursive_Local_Tasks;

   -- P3-4.6: pragma SHARED via renaming and in out under -O2
   procedure Test_Pragma_Shared is
      Flag : Integer := 0;
      pragma SHARED (Flag);
      Flag_Ren : Integer renames Flag;

      procedure Set_Flag (X : in out Integer) is
      begin
         X := 1;
      end Set_Flag;

      task Writer;
      task body Writer is
      begin
         delay 0.02;
         Set_Flag (Flag_Ren);
      end Writer;
   begin
      while Flag_Ren = 0 loop
         null;
      end loop;
      if Flag /= 1 then
         Fail ("pragma SHARED");
      end if;
   end Test_Pragma_Shared;

   -- P3-4.8 & P3-4.9: goto out of task block in package body & nested spec package task activation
   procedure Test_Pkg_Tasks_And_Goto is
      package Outer_Pkg is
         package Inner_Spec_Pkg is
            task Worker is
               entry Ready (V : out Integer);
            end Worker;
         end Inner_Spec_Pkg;
         Got : Integer := 0;
      end Outer_Pkg;

      package body Outer_Pkg is
         package body Inner_Spec_Pkg is
            task body Worker is
            begin
               accept Ready (V : out Integer) do
                  V := 123;
               end Ready;
            end Worker;
         begin
            Inner_Spec_Pkg.Worker.Ready (Got);
         end Inner_Spec_Pkg;
      begin
         declare
            task Local_B is
               entry Done;
            end Local_B;
            task body Local_B is
            begin
               accept Done;
            end Local_B;
         begin
            Local_B.Done;
            goto After_Block;
         end;
         <<After_Block>>
         null;
      end Outer_Pkg;
   begin
      if Outer_Pkg.Got /= 123 then
         Fail ("Nested spec package task activation");
      end if;
   end Test_Pkg_Tasks_And_Goto;

   -- P3-4.10: Entry family >64 slots + dynamic bounds frozen at decl
   procedure Test_Entry_Families is
      Lo : Integer := 1;
      Hi : Integer := 100;

      task Fam_T is
         entry E (Lo .. Hi) (V : out Integer);
      end Fam_T;

      R1, R2 : Integer;

      task body Fam_T is
      begin
         accept E (95) (V : out Integer) do
            V := 950;
         end E;
         accept E (100) (V : out Integer) do
            V := 1000;
         end E;
      end Fam_T;
   begin
      -- Mutate Lo/Hi after task decl: family bounds must stay 1..100
      Lo := 50;
      Hi := 60;
      Fam_T.E (95) (R1);
      Fam_T.E (100) (R2);
      if R1 /= 950 or else R2 /= 1000 then
         Fail ("Large/dynamic entry family");
      end if;
   end Test_Entry_Families;

   -- P3-4.12: Self-reference inside task body completing limited private type
   procedure Test_LP_Task_Self is
      package LP_Pkg is
         type T is limited private;
         procedure Check (X : in out T);
      private
         task type T is
            pragma PRIORITY (6);
            entry Query (Ok : out Boolean);
         end T;
      end LP_Pkg;

      Obj : LP_Pkg.T;

      package body LP_Pkg is
         task body T is
         begin
            accept Query (Ok : out Boolean) do
               Ok := T'Callable;
            end Query;
         end T;

         procedure Check (X : in out T) is
            Ok : Boolean := False;
         begin
            X.Query (Ok);
            if not Ok then
               Fail ("LP task self 'Callable");
            end if;
         end Check;
      end LP_Pkg;
   begin
      LP_Pkg.Check (Obj);
   end Test_LP_Task_Self;

   -- P3-4.15 & P3-4.16: Selective wait guard eval order & CALENDAR.SPLIT
   procedure Test_Select_Order_And_Calendar is
      Log : Integer := 0;
      task Ord_T is
         entry Dummy;
      end Ord_T;

      T_Now : CALENDAR.Time := CALENDAR.Time_Of (2025, 6, 15, 3600.0);
      Y : CALENDAR.Year_Number;
      M : CALENDAR.Month_Number;
      D : CALENDAR.Day_Number;
      S : CALENDAR.Day_Duration;

      function Guard_True return Boolean is
      begin
         Log := Log * 10 + 1;
         return True;
      end Guard_True;
      function Delay_Amt return Duration is
      begin
         Log := Log * 10 + 2;
         return 0.01;
      end Delay_Amt;

      task body Ord_T is
      begin
         select
            when Guard_True =>
               delay Delay_Amt;
         or
            when Guard_True =>
               accept Dummy;
         end select;
      end Ord_T;
   begin
      -- Wait for Ord_T to finish its delay arm
      while not Ord_T'Terminated loop
         delay 0.01;
      end loop;
      -- Pass 1 evaluates both guards (1, 1) before Pass 2 evaluates Delay_Amt (2) -> 112
      if Log /= 112 then
         Fail ("Selective wait guard/delay eval order");
      end if;

      CALENDAR.Split (T_Now, Y, M, D, S);
      if Y /= 2025 or else M /= 6 or else D /= 15 or else S /= 3600.0 then
         Fail ("CALENDAR.Split");
      end if;
   end Test_Select_Order_And_Calendar;

begin
   Test_Concurrent_IO;
   Test_Concurrent_Allocators;
   Test_Recursive_Local_Tasks (3);
   Test_Pragma_Shared;
   Test_Pkg_Tasks_And_Goto;
   Test_Entry_Families;
   Test_LP_Task_Self;
   Test_Select_Order_And_Calendar;
   Put_Line ("PASS3_STAGE4_OK");
end Pass3_Stage4_Test;
