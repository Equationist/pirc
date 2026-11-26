-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Command_Line;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with USERPK; use USERPK;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;
with Pir_Emit;
with Cg_State;
with Cg_Stmt;
with Lib_Binder;

procedure Diana2Pir is
   Target_Triple : Unbounded_String := To_Unbounded_String ("aarch64-apple-darwin");
   Output_Path   : Unbounded_String := Null_Unbounded_String;
   Record_Obj    : Unbounded_String := Null_Unbounded_String;
   Lib_Dir       : Unbounded_String := To_Unbounded_String ("ADA__LIB");
   Enable_Checks : Boolean := True;
   Emit_Main     : Boolean := True;

   I : Natural := 1;
begin
   while I <= Ada.Command_Line.Argument_Count loop
      declare
         Arg : constant String := Ada.Command_Line.Argument (I);
      begin
         if Arg = "--no-checks" then
            Enable_Checks := False;
         elsif Arg = "--checks" then
            Enable_Checks := True;
         elsif Arg = "--no-main" then
            Emit_Main := False;
         elsif Arg = "--main" then
            Emit_Main := True;
         elsif Arg = "--target" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Target_Triple := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 9 and then Arg (Arg'First .. Arg'First + 8) = "--target=" then
            Target_Triple := To_Unbounded_String (Arg (Arg'First + 9 .. Arg'Last));
         elsif Arg = "--record-obj" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Record_Obj := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 13 and then Arg (Arg'First .. Arg'First + 12) = "--record-obj=" then
            Record_Obj := To_Unbounded_String (Arg (Arg'First + 13 .. Arg'Last));
         elsif Arg = "--lib" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Lib_Dir := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 6 and then Arg (Arg'First .. Arg'First + 5) = "--lib=" then
            Lib_Dir := To_Unbounded_String (Arg (Arg'First + 6 .. Arg'Last));
         elsif Arg = "-o" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Output_Path := To_Unbounded_String (Ada.Command_Line.Argument (I));
         else
            Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "diana2pir: unknown option: " & Arg);
            Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
            return;
         end if;
      end;
      I := I + 1;
   end loop;

   OPEN_DIANA;
   declare
      User_Root : constant TREE := D (XD_USER_ROOT, CONST_ROOT);
      Structure : constant TREE := D (XD_STRUCTURE, User_Root);
      Err_Cnt   : constant Integer := DI (XD_ERR_COUNT, CONST_ROOT);
      Ctx       : Cg_State.Context;
   begin
      if Err_Cnt > 0 then
         Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error,
           "diana2pir: aborting due to" & Integer'Image (Err_Cnt) & " frontend error(s)");
         CLOSE_DIANA;
         Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
         return;
      end if;

      declare
         function Trim_Img (N : Natural) return String is
            S : constant String := Natural'Image (N);
         begin
            return S (S'First + 1 .. S'Last);
         end Trim_Img;

         CU_Count : Natural := 0;
         SL_Cnt   : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Structure));
         Tmp_CU   : TREE;
      begin
         while not IS_EMPTY (SL_Cnt) loop
            POP (SL_Cnt, Tmp_CU);
            CU_Count := CU_Count + 1;
         end loop;

         -- P3-5.12: In library mode with multiple CUs in one source file, emit one .pir/.o per CU
         -- so partial superseding on later recompilation never causes duplicate-symbol link errors!
         if not Emit_Main and then Length (Record_Obj) > 0 and then CU_Count > 1 then
            declare
               Out_S   : constant String := To_String (Output_Path);
               Out_Base : constant String :=
                 (if Out_S'Length > 4 and then Out_S (Out_S'Last - 3 .. Out_S'Last) = ".pir"
                  then Out_S (Out_S'First .. Out_S'Last - 4) else Out_S);
            begin
               for CU_I in 1 .. CU_Count loop
                  declare
                     CU_Ctx : Cg_State.Context;
                  begin
                     Cg_State.Init_Context
                       (Ctx           => CU_Ctx,
                        Target_Triple => To_String (Target_Triple),
                        Enable_Checks => Enable_Checks,
                        Emit_Main     => False);
                     Cg_Stmt.Emit_Compilation (CU_Ctx, Structure, Only_CU => CU_I);
                     Pir_Emit.Write_Module (CU_Ctx.E, Out_Base & "__cu" & Trim_Img (CU_I) & ".pir");
                  end;
               end loop;
               Lib_Binder.Record_Compilation_Units (Structure, To_String (Record_Obj), To_String (Lib_Dir), Per_CU_Obj => True);
            end;
         else
            Cg_State.Init_Context
              (Ctx           => Ctx,
               Target_Triple => To_String (Target_Triple),
               Enable_Checks => Enable_Checks,
               Emit_Main     => Emit_Main);

            Cg_Stmt.Emit_Compilation (Ctx, Structure);
            Pir_Emit.Write_Module (Ctx.E, To_String (Output_Path));
            if Length (Record_Obj) > 0 then
               Lib_Binder.Record_Compilation_Units (Structure, To_String (Record_Obj), To_String (Lib_Dir));
            end if;
         end if;
      end;
   end;
   CLOSE_DIANA;
exception
   when Cg_Stmt.Elab_Cycle_Error =>
      CLOSE_DIANA;
      Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
end Diana2Pir;
