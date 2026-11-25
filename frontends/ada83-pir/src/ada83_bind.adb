-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

-- ada83_bind — Ada 83 Program Library Binder (LRM Chapter 10)
-- Validates library consistency (LRM 10.3/10.5), selects live .o files,
-- and emits binder_main.pir with topological elaboration order.

with Ada.Command_Line;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with Lib_Binder;

procedure Ada83_Bind is
   Lib_Dir       : Unbounded_String := To_Unbounded_String ("ADA__LIB");
   Main_Unit     : Unbounded_String := Null_Unbounded_String;
   Main_Obj      : Unbounded_String := Null_Unbounded_String;
   Target_Triple : Unbounded_String := To_Unbounded_String ("aarch64-apple-darwin");
   Pir_Out       : Unbounded_String := Null_Unbounded_String;
   Objs_Out      : Unbounded_String := Null_Unbounded_String;
   Check_Only    : Boolean := False;
   Skip_Checks   : Boolean := False;
   OK            : Boolean := False;

   I : Natural := 1;
begin
   while I <= Ada.Command_Line.Argument_Count loop
      declare
         Arg : constant String := Ada.Command_Line.Argument (I);
      begin
         if Arg = "--check-only" then
            Check_Only := True;
         elsif Arg = "--skip-checks" then
            Skip_Checks := True;
         elsif Arg = "--lib" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Lib_Dir := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 6 and then Arg (Arg'First .. Arg'First + 5) = "--lib=" then
            Lib_Dir := To_Unbounded_String (Arg (Arg'First + 6 .. Arg'Last));
         elsif Arg = "--main" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Main_Unit := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 7 and then Arg (Arg'First .. Arg'First + 6) = "--main=" then
            Main_Unit := To_Unbounded_String (Arg (Arg'First + 7 .. Arg'Last));
         elsif Arg = "--main-obj" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Main_Obj := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 11 and then Arg (Arg'First .. Arg'First + 10) = "--main-obj=" then
            Main_Obj := To_Unbounded_String (Arg (Arg'First + 11 .. Arg'Last));
         elsif Arg = "--target" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Target_Triple := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 9 and then Arg (Arg'First .. Arg'First + 8) = "--target=" then
            Target_Triple := To_Unbounded_String (Arg (Arg'First + 9 .. Arg'Last));
         elsif Arg = "--emit-pir" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Pir_Out := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 11 and then Arg (Arg'First .. Arg'First + 10) = "--emit-pir=" then
            Pir_Out := To_Unbounded_String (Arg (Arg'First + 11 .. Arg'Last));
         elsif Arg = "--objs-out" and then I + 1 <= Ada.Command_Line.Argument_Count then
            I := I + 1;
            Objs_Out := To_Unbounded_String (Ada.Command_Line.Argument (I));
         elsif Arg'Length > 11 and then Arg (Arg'First .. Arg'First + 10) = "--objs-out=" then
            Objs_Out := To_Unbounded_String (Arg (Arg'First + 11 .. Arg'Last));
         else
            Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "ada83_bind: unknown option: " & Arg);
            Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
            return;
         end if;
      end;
      I := I + 1;
   end loop;

   Lib_Binder.Bind_Library
     (Lib_Dir       => To_String (Lib_Dir),
      Main_Unit     => To_String (Main_Unit),
      Target_Triple => To_String (Target_Triple),
      Pir_Out_Path  => To_String (Pir_Out),
      Objs_Out_Path => To_String (Objs_Out),
      Check_Only    => Check_Only,
      Skip_Checks   => Skip_Checks,
      Success       => OK,
      Main_Obj      => To_String (Main_Obj));

   if not OK then
      Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
   end if;
end Ada83_Bind;
