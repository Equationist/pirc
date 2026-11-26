-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

-- ada83_bind — Ada 83 Program Library Binder (LRM Chapter 10)
-- Validates library consistency (LRM 10.3/10.5), selects live .o files,
-- and emits binder_main.pir with topological elaboration order.

with Ada.Command_Line;
with Ada.Exceptions;
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

   procedure Print_Usage is
   begin
      Ada.Text_IO.Put_Line
        (Ada.Text_IO.Standard_Error,
         "Usage: ada83_bind [--lib=<dir>] [--main=<unit>] [--main-obj=<o>] " &
         "[--target=<triple>] [--emit-pir=<pir>] [--objs-out=<txt>] [--check-only] [--skip-checks]");
   end Print_Usage;

   function Take_Opt_Arg (Opt_Name : String; Out_Val : out Unbounded_String) return Boolean is
   begin
      if I + 1 > Ada.Command_Line.Argument_Count then
         Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "ada83_bind: missing value for " & Opt_Name);
         Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
         Out_Val := Null_Unbounded_String;
         return False;
      end if;
      I := I + 1;
      Out_Val := To_Unbounded_String (Ada.Command_Line.Argument (I));
      return True;
   end Take_Opt_Arg;
begin
   while I <= Ada.Command_Line.Argument_Count loop
      declare
         Arg : constant String := Ada.Command_Line.Argument (I);
      begin
         if Arg = "-h" or else Arg = "--help" then
            Print_Usage;
            Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Success);
            return;
         elsif Arg = "--check-only" then
            Check_Only := True;
         elsif Arg = "--skip-checks" then
            Skip_Checks := True;
         elsif Arg = "--lib" then
            if not Take_Opt_Arg ("--lib", Lib_Dir) then return; end if;
         elsif Arg'Length >= 6 and then Arg (Arg'First .. Arg'First + 5) = "--lib=" then
            if Arg'Length = 6 then
               Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "ada83_bind: empty value for --lib=");
               Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
               return;
            end if;
            Lib_Dir := To_Unbounded_String (Arg (Arg'First + 6 .. Arg'Last));
         elsif Arg = "--main" then
            if not Take_Opt_Arg ("--main", Main_Unit) then return; end if;
         elsif Arg'Length >= 7 and then Arg (Arg'First .. Arg'First + 6) = "--main=" then
            Main_Unit := To_Unbounded_String (Arg (Arg'First + 7 .. Arg'Last));
         elsif Arg = "--main-obj" then
            if not Take_Opt_Arg ("--main-obj", Main_Obj) then return; end if;
         elsif Arg'Length >= 11 and then Arg (Arg'First .. Arg'First + 10) = "--main-obj=" then
            Main_Obj := To_Unbounded_String (Arg (Arg'First + 11 .. Arg'Last));
         elsif Arg = "--target" then
            if not Take_Opt_Arg ("--target", Target_Triple) then return; end if;
         elsif Arg'Length >= 9 and then Arg (Arg'First .. Arg'First + 8) = "--target=" then
            if Arg'Length = 9 then
               Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "ada83_bind: empty value for --target=");
               Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
               return;
            end if;
            Target_Triple := To_Unbounded_String (Arg (Arg'First + 9 .. Arg'Last));
         elsif Arg = "--emit-pir" then
            if not Take_Opt_Arg ("--emit-pir", Pir_Out) then return; end if;
         elsif Arg'Length >= 11 and then Arg (Arg'First .. Arg'First + 10) = "--emit-pir=" then
            Pir_Out := To_Unbounded_String (Arg (Arg'First + 11 .. Arg'Last));
         elsif Arg = "--objs-out" then
            if not Take_Opt_Arg ("--objs-out", Objs_Out) then return; end if;
         elsif Arg'Length >= 11 and then Arg (Arg'First .. Arg'First + 10) = "--objs-out=" then
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
exception
   when E : others =>
      Ada.Text_IO.Put_Line
        (Ada.Text_IO.Standard_Error,
         "ada83_bind: internal error: " & Ada.Exceptions.Exception_Information (E));
      Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
end Ada83_Bind;
