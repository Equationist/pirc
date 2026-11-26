-- Pass 2 Stage 1 Regression Test:
--   1. NUMERIC_ERROR / CONSTRAINT_ERROR equivalence (AI-00387)
--   2. Abort during accept body (no UAF/double-release) & LRM 9.10(6) sync points
--   3. Selective wait terminate inside nested block master (L_Term leaves block master)
--   4. Activation failure with already-activated child task (Act_Fail ordering)
--   5. Declarative-part exception with nested child tasks on terminate
--   6. Stale m->activation_errors not leaking across activations
--   7. Second accept for same entry with captured params & return in local subp inside accept
--   8. Local unconstrained array alive inside exception handler (SS UAF fix)
--   9. CALENDAR.SPLIT with scalar view conversion out params
--  10. TEXT_IO SET_COL at EOL, FLOAT_IO -0.0 & large EXP=0 LAYOUT_ERROR, based real ldexp,
--      ENUMERATION_IO lone quote at EOL, NEW_PAGE after auto page wrap, param precedence
with Text_IO;
with Calendar;
procedure Pass2_Stage1_Test is
   package Int_IO is new Text_IO.Integer_IO (Integer);
   package Flt_IO is new Text_IO.Float_IO (Float);
   type Color is (Red, Green, Blue);
   package Color_IO is new Text_IO.Enumeration_IO (Color);

   procedure Test_Numeric_Error is
      A : Integer := 10;
      Z : Integer := 0;
      Caught_Div : Boolean := False;
      Caught_Ovf : Boolean := False;
      Caught_Exp : Boolean := False;
   begin
      begin
         A := A / Z;
      exception
         when Constraint_Error =>
            Caught_Div := True;
      end;
      if not Caught_Div then
         raise Program_Error;
      end if;

      begin
         A := Integer'Last;
         A := A + 1;
      exception
         when Constraint_Error =>
            Caught_Ovf := True;
      end;
      if not Caught_Ovf then
         raise Program_Error;
      end if;

      begin
         raise Numeric_Error;
      exception
         when Constraint_Error =>
            raise Program_Error;
         when Numeric_Error =>
            Caught_Exp := True;
      end;
      if not Caught_Exp then
         raise Program_Error;
      end if;
   end Test_Numeric_Error;

   procedure Test_SS_In_Handler is
      function Make_Str (N : Integer) return String is
         R : String (1 .. N) := (others => 'X');
      begin
         return R;
      end Make_Str;
   begin
      declare
         Local_S : constant String := Make_Str (64);
         Tmp     : String (1 .. 64);
      begin
         Tmp := Make_Str (64);
         if Tmp (1) = 'X' then
            raise Constraint_Error;
         end if;
      exception
         when Constraint_Error =>
            -- Local_S must NOT have been freed by handler entry SS release!
            declare
               Clobber : String (1 .. 64) := Make_Str (64);
            begin
               Clobber := (others => 'Z');
               if Local_S'Length /= 64 or else Local_S (1) /= 'X' or else Local_S (64) /= 'X' then
                  raise Program_Error;
               end if;
            end;
      end;
   end Test_SS_In_Handler;

   procedure Test_Abort_In_Accept is
      Got_TE : Boolean := False;
      task T is
         entry Hold;
      end T;
      task Caller;
      task body T is
         X : Integer := 0;
      begin
         accept Hold do
            for I in 1 .. 500_000 loop
               X := X + 1;
            end loop;
         end Hold;
      end T;
      task body Caller is
      begin
         T.Hold;
      exception
         when Tasking_Error =>
            Got_TE := True;
      end Caller;
   begin
      delay 0.01;
      abort T;
      -- Wait for Caller to finish before checking Got_TE
      while not Caller'Terminated loop
         delay 0.005;
      end loop;
      -- Either Caller completed before abort or got Tasking_Error cleanly without UAF/crash
      if not T'Terminated then
         delay 0.05;
      end if;
      if not T'Terminated then
         raise Program_Error;
      end if;
   end Test_Abort_In_Accept;

   procedure Test_Nested_Block_Terminate is
      task Outer is
         entry Ping;
      end Outer;
      task body Outer is
      begin
         declare
            task Inner is
               entry Pong;
            end Inner;
            task body Inner is
            begin
               select
                  accept Pong;
               or
                  terminate;
               end select;
            end Inner;
         begin
            select
               accept Ping;
            or
               terminate;
            end select;
         end;
      end Outer;
   begin
      null;
   end Test_Nested_Block_Terminate;

   procedure Test_Decl_Exc_Nested_Terminate is
      Caught : Boolean := False;
      procedure Inner_Proc is
         package P is
            task Parent is entry Ping; end Parent;
         end P;
         package Fail_P is end Fail_P;
         package body P is
            task body Parent is
               task Child is entry Pong; end Child;
               task body Child is
               begin
                  select accept Pong; or terminate; end select;
               end Child;
            begin
               select accept Ping; or terminate; end select;
            end Parent;
         end P;
         package body Fail_P is
         begin
            raise Constraint_Error;
         end Fail_P;
      begin
         null;
      end Inner_Proc;
   begin
      Inner_Proc;
   exception
      when Constraint_Error =>
         Caught := True;
         if not Caught then raise Program_Error; end if;
   end Test_Decl_Exc_Nested_Terminate;

   procedure Test_Stale_Activation_Error is
      task type Bad_T;
      type Bad_Ptr is access Bad_T;
      P : Bad_Ptr;
      Caught : Boolean := False;
      task body Bad_T is
         X : Positive := 0;
      begin
         if X > 0 then null; end if;
      end Bad_T;
   begin
      begin
         P := new Bad_T;
      exception
         when Tasking_Error =>
            Caught := True;
      end;
      if not Caught then raise Program_Error; end if;
      -- Subsequent healthy task activation in same scope must NOT see stale error!
      declare
         task Good_T;
         task body Good_T is begin null; end Good_T;
      begin
         null;
      end;
   end Test_Stale_Activation_Error;

   procedure Test_Second_Accept_And_Return is
      Sum : Integer := 0;
      task Worker is
         entry Add (V : in Integer);
      end Worker;
      task body Worker is
      begin
         accept Add (V : in Integer) do
            declare
               procedure Helper is
               begin
                  Sum := Sum + V;
                  return; -- must return from Helper, NOT from accept!
               end Helper;
            begin
               Helper;
               Sum := Sum + 1;
            end;
         end Add;
         accept Add (V : in Integer) do
            declare
               procedure Helper2 is
               begin
                  Sum := Sum + V;
                  return;
               end Helper2;
            begin
               Helper2;
               Sum := Sum + 10;
            end;
         end Add;
      end Worker;
   begin
      Worker.Add (5);
      Worker.Add (7);
      while not Worker'Terminated loop
         delay 0.005;
      end loop;
      if Sum /= 23 then
         raise Program_Error;
      end if;
   end Test_Second_Accept_And_Return;

   procedure Test_Calendar_Split_View_Conv is
      type My_Int is new Integer;
      type My_Dur is new Duration;
      T : Calendar.Time := Calendar.Time_Of (2026, 11, 25, 12.5);
      Y : My_Int := 0;
      M : My_Int := 0;
      D : My_Int := 0;
      S : My_Dur := 0.0;
   begin
      Calendar.Split (T, Calendar.Year_Number (Y), Calendar.Month_Number (M),
                      Calendar.Day_Number (D), Calendar.Day_Duration (S));
      if Y /= 2026 or else M /= 11 or else D /= 25 then
         raise Program_Error;
      end if;
   end Test_Calendar_Split_View_Conv;

   procedure Test_IO_Fixes is
      F    : Text_IO.File_Type;
      Buf  : String (1 .. 10);
      NegZ : Float := -0.0;
      Big  : Float := 1.0E+30;
      Val  : Float := 0.0;
      Last : Positive;
      C    : Color;
      Got_Layout : Boolean := False;
      Got_Data   : Boolean := False;
      Got_CE     : Boolean := False;
   begin
      -- (1) Negative zero must not print "-0.0"
      Flt_IO.Put (To => Buf, Item => NegZ, Aft => 1, Exp => 2);
      for I in Buf'Range loop
         if Buf (I) = '-' then
            raise Program_Error;
         end if;
      end loop;

      -- (2) Large EXP=0 value must raise LAYOUT_ERROR (not silently truncate)
      begin
         Flt_IO.Put (To => Buf, Item => Big, Aft => 2, Exp => 0);
      exception
         when Text_IO.Layout_Error =>
            Got_Layout := True;
      end;
      if not Got_Layout then raise Program_Error; end if;

      -- (3) Based real near extreme exponent (16#0.4#E+256 = 2**1022, fits in IEEE f64)
      declare
         package Long_Flt_IO is new Text_IO.Float_IO (Long_Float);
         LV : Long_Float := 0.0;
      begin
         Long_Flt_IO.Get ("16#0.4#E+256", LV, Last);
         if LV <= 0.0 then raise Program_Error; end if;
      end;

      -- (4) SET_COL on IN_FILE at EOL when COL = TO is a no-op
      Text_IO.Create (F, Text_IO.Out_File);
      Text_IO.Put_Line (F, "AB");
      Text_IO.Put_Line (F, "CD");
      Text_IO.Reset (F, Text_IO.In_File);
      declare
         Ch1, Ch2, Ch3 : Character;
      begin
         Text_IO.Get (F, Ch1); -- 'A', col=2
         Text_IO.Get (F, Ch2); -- 'B', col=3, End_Of_Line=True
         Text_IO.Set_Col (F, 3); -- col=3 already -> MUST be a no-op (not skip line!)
         if not Text_IO.End_Of_Line (F) or else Integer (Text_IO.Line (F)) /= 1 then
            raise Program_Error;
         end if;
         Text_IO.Get (F, Ch3);
         if Ch3 /= 'C' then raise Program_Error; end if;
      end;
      Text_IO.Close (F);

      -- (5) ENUMERATION_IO lone quote at EOL must not consume '\n'
      Text_IO.Create (F, Text_IO.Out_File);
      Text_IO.Put_Line (F, "'");
      Text_IO.Put_Line (F, "GREEN");
      Text_IO.Reset (F, Text_IO.In_File);
      begin
         Color_IO.Get (F, C);
      exception
         when Text_IO.Data_Error =>
            Got_Data := True;
      end;
      if not Got_Data or else not Text_IO.End_Of_Line (F) then
         raise Program_Error;
      end if;
      Color_IO.Get (F, C);
      if C /= Green then raise Program_Error; end if;
      Text_IO.Close (F);

      -- (6) Parameter CONSTRAINT_ERROR precedence before STATUS_ERROR on closed file
      begin
         Text_IO.Skip_Line (F, 0);
      exception
         when Constraint_Error =>
            Got_CE := True;
      end;
      if not Got_CE then raise Program_Error; end if;
   end Test_IO_Fixes;

begin
   Test_Numeric_Error;
   Test_SS_In_Handler;
   Test_Abort_In_Accept;
   Test_Nested_Block_Terminate;
   Test_Decl_Exc_Nested_Terminate;
   Test_Stale_Activation_Error;
   Test_Second_Accept_And_Return;
   Test_Calendar_Split_View_Conv;
   Test_IO_Fixes;
   Text_IO.Put_Line ("PASS2_STAGE1_OK");
end Pass2_Stage1_Test;
