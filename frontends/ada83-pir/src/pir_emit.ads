-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;

package Pir_Emit is

   type Emitter is limited private;

   procedure Init (E : in out Emitter; Target_Triple : String);

   -- Module-level sections
   procedure Add_Type_Decl (E : in out Emitter; Decl : String);
   procedure Add_Sig_Decl (E : in out Emitter; Decl : String);
   procedure Add_Data_Decl (E : in out Emitter; Name : String; Decl : String);
   procedure Add_Global_Decl (E : in out Emitter; Name : String; Decl : String);
   procedure Add_Extern_Func (E : in out Emitter; Name : String; Decl : String);
   function Has_Symbol (E : Emitter; Name : String) return Boolean;
   function Has_Type (E : Emitter; Name : String) return Boolean;

   -- Function-level buffering (supports nested function emission via save/restore)
   type Func_Buffer is private;

   procedure Begin_Func
     (E           : in out Emitter;
      Saved       : out Func_Buffer;
      Header_Line : String);

   procedure End_Func
     (E     : in out Emitter;
      Saved : in out Func_Buffer);

   function Next_Reg (E : in out Emitter; Prefix : String := "t") return String;
   function Next_Label (E : in out Emitter; Prefix : String := "L") return String;
   function Next_Sym_Id (E : in out Emitter) return Natural;

   -- Emit an alloca into the current function's .entry block
   procedure Emit_Entry_Alloca (E : in out Emitter; Line : String);

   -- Emit into the current basic block
   procedure Emit_Label (E : in out Emitter; Label : String; Params : String := "");
   procedure Emit_Inst (E : in out Emitter; Line : String);
   procedure Emit_Term (E : in out Emitter; Line : String);
   procedure Emit_Comment (E : in out Emitter; Text : String);

   function Is_Terminated (E : Emitter) return Boolean;
   procedure Set_Terminated (E : in out Emitter; Val : Boolean);

   -- String escaping for PIR data literals
   function Escape_Pir_String (S : String) return String;
   function Img (V : Long_Long_Integer) return String;
   function Img (V : Integer) return String;

   -- Write complete PIR module to stdout or a file
   procedure Write_Module (E : Emitter; Output_Path : String := "");

private

   type Func_Buffer is record
      Header       : Unbounded_String;
      Entry_Alloc  : Unbounded_String;
      Entry_Body   : Unbounded_String;
      Rest_Body    : Unbounded_String;
      In_Entry     : Boolean := True;
      Terminated   : Boolean := False;
      Reg_Counter  : Natural := 0;
      Lbl_Counter  : Natural := 0;
   end record;

   type Emitter is limited record
      Target      : Unbounded_String;
      Types_Buf   : Unbounded_String;
      Sigs_Buf    : Unbounded_String;
      Data_Buf    : Unbounded_String;
      Globals_Buf : Unbounded_String;
      Externs_Buf : Unbounded_String;
      Funcs_Buf   : Unbounded_String;
      Sym_Set     : Unbounded_String; -- pipe-delimited set of declared symbols
      Def_Funcs   : Unbounded_String; -- pipe-delimited set of defined function symbols
      Type_Set    : Unbounded_String; -- pipe-delimited set of declared type names
      Cur         : Func_Buffer;
      Sym_Counter : Natural := 0;
   end record;

end Pir_Emit;
