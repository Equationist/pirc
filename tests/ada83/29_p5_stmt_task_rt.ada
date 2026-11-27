-- Pass-5 regression test: R5 (Subp snapshots/handlers/captures), R7 (Runtime concurrency), R8 (Valid DIANA legality)
with Text_IO; use Text_IO;

procedure P5_Stmt_Task_RT is
   Failed : Integer := 0;
   procedure Check (Cond : Boolean; Label : String);

   -- R8: Private scalar static choices + pragma inside record & param list + access OUT 'Size/'Address
   package Priv_Scalar is
      type P_Code is private;
      C1 : constant P_Code;
      C2 : constant P_Code;
      function Classify (X : P_Code) return Integer;
   private
      type P_Code is range 1 .. 4;
      subtype P_Low is P_Code range 1 .. 2;
      C1 : constant P_Code := 1;
      C2 : constant P_Code := 3;
   end Priv_Scalar;

   type Pragma_Rec is record
      X : Integer := 5;
      pragma List (On);
      Y : Integer := 6;
   end record;

   type Int_Ptr is access Integer;

   -- R8: Character literals in instantiated generic enumeration type
   generic
      Offset : Integer;
   package Gen_Char_Der is
      type Local_Chars is ('a', 'b', 'c');
      type Der_Enum is new Local_Chars;
      function First_Val return Der_Enum;
   end Gen_Char_Der;

   -- R5: Nested generic inside generic package body + multi-inst capture
   generic
      Base : Integer;
   package Outer_Gen is
      function Run (X : Integer) return Integer;
   end Outer_Gen;

   P  : Int_Ptr;
   Sz : Integer := 0;
   PR : Pragma_Rec;

   procedure Check (Cond : Boolean; Label : String) is
   begin
      if not Cond then
         Put_Line ("FAIL: " & Label);
         Failed := Failed + 1;
      end if;
   end Check;

   package body Priv_Scalar is
      function Classify (X : P_Code) return Integer is
      begin
         case X is
            when P_Low => return 10;
            when others => return 20;
         end case;
      end Classify;
   end Priv_Scalar;

   procedure Check_Out_Access_Attrs (P : out Int_Ptr; Sz : out Integer) is
   begin
      Sz := P'Size;
      P := new Integer'(99);
   end Check_Out_Access_Attrs;

   package body Gen_Char_Der is
      function First_Val return Der_Enum is
      begin
         return Der_Enum'First;
      end First_Val;
   end Gen_Char_Der;

   package Inst_Chars is new Gen_Char_Der (1);
   use Inst_Chars;

   -- R5: Local package body exception handler must NOT clobber enclosing dynamic array!
   procedure Test_Local_Pkg_Handler (N : Integer) is
      type Dyn_A is array (1 .. N) of Integer;
      Outer_A : Dyn_A := (others => 77);
      Handled : Boolean := False;
      package Inner_P is
         Val : Integer := 0;
      end Inner_P;
      package body Inner_P is
         Inner_A : Dyn_A := (others => 33);
         My_Err  : exception;
      begin
         Val := Inner_A (N);
         raise My_Err;
      exception
         when My_Err =>
            Handled := True;
            Val := Inner_A (1) + 1;
      end Inner_P;
   begin
      Check (Handled and then Inner_P.Val = 34, "local_pkg_hdl_ok");
      Check (Outer_A (1) = 77 and then Outer_A (N) = 77, "local_pkg_outer_dyn_intact");
   end Test_Local_Pkg_Handler;

   -- R5: Case statement with static discrete subtype
   function Case_Range_Attr (X : Integer) return Integer is
      subtype Sub_R is Integer range 10 .. 12;
   begin
      case X is
         when Sub_R  => return 1;
         when others => return 2;
      end case;
   end Case_Range_Attr;

   package body Outer_Gen is
      Local_Bias : Integer := Base * 2;
      generic
         Delta_V : Integer;
      function Inner_Gen (V : Integer) return Integer;
      function Inner_Gen (V : Integer) return Integer is
      begin
         return V + Local_Bias + Delta_V;
      end Inner_Gen;
      function Add_One is new Inner_Gen (1);
      function Run (X : Integer) return Integer is
      begin
         return Add_One (X);
      end Run;
   end Outer_Gen;

   package OG1 is new Outer_Gen (5);
   package OG2 is new Outer_Gen (10);

   -- R5 + R8: Derived task type with dynamic entry family + access-to-task P'Storage_Size + dynamic Storage_Size uplevel capture
   procedure Test_Tasks (Fam_Hi : Integer) is
      SS_Bytes : Integer := 65536;
      task type Base_T is
         entry Ping (1 .. Fam_Hi) (V : in out Integer);
         entry Pong (V : in out Integer);
      end Base_T;
      for Base_T'Storage_Size use SS_Bytes;

      type Der_T is new Base_T;
      type Acc_T is access Base_T;

      DT : Der_T;
      Y1 : Integer := 10;
      Y2 : Integer := 20;
      Y3 : Integer := 30;

      task Caller2;

      task body Base_T is
      begin
         accept Ping (1) (V : in out Integer) do
            -- Nested accept on distinct entry tests Temp_Var_Scope preservation
            accept Pong (V : in out Integer) do
               V := V + 100;
            end Pong;
            V := V + 1;
         end Ping;
         accept Ping (2) (V : in out Integer) do
            V := V + 200;
         end Ping;
      end Base_T;

      function Make_And_Check return Integer is
         PT : Acc_T := new Base_T;
         SS : constant Integer := PT'Storage_Size;
         X1 : Integer := 5;
         X2 : Integer := 7;
         X3 : Integer := 9;
         -- Chained local task-entry renaming called from nested task
         procedure Ren1 (V : in out Integer) renames PT.Pong;
         procedure Ren2 (V : in out Integer) renames Ren1;
         task Helper;
         task body Helper is
         begin
            delay 0.01;
            Ren2 (X2);
         end Helper;
      begin
         PT.Ping (1) (X1);
         PT.Ping (2) (X3);
         if SS > 0 and then X1 = 6 and then X2 = 107 and then X3 = 209 then
            return 1;
         else
            return 0;
         end if;
      end Make_And_Check;

      task body Caller2 is
      begin
         delay 0.01;
         DT.Pong (Y2);
      end Caller2;
   begin
      Check (Make_And_Check = 1, "acc_task_ss_chained_ren");
      DT.Ping (1) (Y1);
      DT.Ping (2) (Y3);
      delay 0.02;
      Check (Y1 = 11 and then Y2 = 120 and then Y3 = 230, "derived_dyn_entry_family");
   end Test_Tasks;

   -- R7: Concurrent task stress (Text_IO + delay + rendezvous)
   procedure Test_Concurrent_RT is
      task type Worker is
         entry Start (Id : Integer);
         entry Done (Res : out Integer);
      end Worker;
      Ws : array (1 .. 4) of Worker;
      Sum : Integer := 0;
      R   : Integer;
      task body Worker is
         My_Id : Integer := 0;
      begin
         accept Start (Id : Integer) do
            My_Id := Id;
         end Start;
         delay 0.005;
         accept Done (Res : out Integer) do
            Res := My_Id * 10;
         end Done;
      end Worker;
   begin
      for I in Ws'Range loop
         Ws (I).Start (I);
      end loop;
      for I in Ws'Range loop
         Ws (I).Done (R);
         Sum := Sum + R;
      end loop;
      Check (Sum = 100, "concurrent_rt_sum");
   end Test_Concurrent_RT;
begin
   Check (Priv_Scalar.Classify (Priv_Scalar.C1) = 10 and then Priv_Scalar.Classify (Priv_Scalar.C2) = 20, "priv_scalar_case");
   Check (PR.X = 5 and then PR.Y = 6, "pragma_rec");
   Check_Out_Access_Attrs (P, Sz);
   Check (Sz > 0 and then P.all = 99, "out_access_size");
   Check (Inst_Chars.First_Val = 'a', "gen_char_lit_der");
   Test_Local_Pkg_Handler (8);
   Check (Case_Range_Attr (11) = 1 and then Case_Range_Attr (15) = 2, "case_range_attr");
   Check (OG1.Run (3) = 14 and then OG2.Run (3) = 24, "nested_gen_in_body");
   Test_Tasks (2);
   Test_Concurrent_RT;

   if Failed = 0 then
      Put_Line ("P5_STMT_TASK_RT_OK");
   else
      Put_Line ("P5_STMT_TASK_RT_FAILED");
   end if;
end P5_Stmt_Task_RT;
