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

procedure Diana2Pir is
   Target_Triple : Unbounded_String := To_Unbounded_String ("aarch64-apple-darwin");
   Output_Path   : Unbounded_String := Null_Unbounded_String;
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

      Cg_State.Init_Context
        (Ctx           => Ctx,
         Target_Triple => To_String (Target_Triple),
         Enable_Checks => Enable_Checks,
         Emit_Main     => Emit_Main);

      Cg_Stmt.Emit_Compilation (Ctx, Structure);
      Pir_Emit.Write_Module (Ctx.E, To_String (Output_Path));
   end;
   CLOSE_DIANA;
end Diana2Pir;
