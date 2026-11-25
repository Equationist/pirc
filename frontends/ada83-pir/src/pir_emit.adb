-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Fixed;
with Ada.Text_IO;

package body Pir_Emit is

   NL : constant Character := ASCII.LF;

   procedure Init (E : in out Emitter; Target_Triple : String) is
   begin
      E.Target      := To_Unbounded_String (Target_Triple);
      E.Types_Buf   := Null_Unbounded_String;
      E.Sigs_Buf    := Null_Unbounded_String;
      E.Data_Buf    := Null_Unbounded_String;
      E.Globals_Buf := Null_Unbounded_String;
      E.Externs_Buf := Null_Unbounded_String;
      E.Funcs_Buf   := Null_Unbounded_String;
      E.Sym_Set     := To_Unbounded_String ("|");
      E.Def_Funcs   := To_Unbounded_String ("|");
      E.Type_Set    := To_Unbounded_String ("|");
      E.Sym_Counter := 0;
   end Init;

   function Has_Symbol (E : Emitter; Name : String) return Boolean is
   begin
      return Index (E.Sym_Set, "|" & Name & "|") > 0;
   end Has_Symbol;

   procedure Mark_Symbol (E : in out Emitter; Name : String) is
   begin
      if not Has_Symbol (E, Name) then
         Append (E.Sym_Set, Name & "|");
      end if;
   end Mark_Symbol;

   function Has_Type (E : Emitter; Name : String) return Boolean is
   begin
      return Index (E.Type_Set, "|" & Name & "|") > 0;
   end Has_Type;

   procedure Add_Type_Decl (E : in out Emitter; Decl : String) is
      -- Extract "@name" from "type @name = ..."
      At_Pos : constant Natural := Ada.Strings.Fixed.Index (Decl, "@");
      Eq_Pos : constant Natural := Ada.Strings.Fixed.Index (Decl, "=");
   begin
      if At_Pos > 0 and then Eq_Pos > At_Pos then
         declare
            TName : constant String :=
              Ada.Strings.Fixed.Trim (Decl (At_Pos .. Eq_Pos - 1), Ada.Strings.Both);
         begin
            if Has_Type (E, TName) then
               return;
            end if;
            Append (E.Type_Set, TName & "|");
         end;
      end if;
      Append (E.Types_Buf, Decl & NL);
   end Add_Type_Decl;

   procedure Add_Sig_Decl (E : in out Emitter; Decl : String) is
   begin
      Append (E.Sigs_Buf, Decl & NL);
   end Add_Sig_Decl;

   procedure Add_Data_Decl (E : in out Emitter; Name : String; Decl : String) is
   begin
      if Has_Symbol (E, Name) then
         return;
      end if;
      Mark_Symbol (E, Name);
      Append (E.Data_Buf, Decl & NL);
   end Add_Data_Decl;

   procedure Add_Global_Decl (E : in out Emitter; Name : String; Decl : String) is
   begin
      if Has_Symbol (E, Name) then
         return;
      end if;
      Mark_Symbol (E, Name);
      Append (E.Globals_Buf, Decl & NL);
   end Add_Global_Decl;

   procedure Add_Extern_Func (E : in out Emitter; Name : String; Decl : String) is
   begin
      if Has_Symbol (E, Name) then
         return;
      end if;
      Mark_Symbol (E, Name);
      Append (E.Externs_Buf, Decl & NL);
   end Add_Extern_Func;

   procedure Begin_Func
     (E           : in out Emitter;
      Saved       : out Func_Buffer;
      Header_Line : String)
   is
   begin
      Saved := E.Cur;
      E.Cur := (Header      => To_Unbounded_String (Header_Line),
                Entry_Alloc => Null_Unbounded_String,
                Entry_Body  => Null_Unbounded_String,
                Rest_Body   => Null_Unbounded_String,
                In_Entry    => True,
                Terminated  => False,
                Reg_Counter => 0,
                Lbl_Counter => 0);
   end Begin_Func;

   procedure End_Func
     (E     : in out Emitter;
      Saved : in out Func_Buffer)
   is
      Hdr    : constant String := To_String (E.Cur.Header);
      At_Pos : constant Natural := Ada.Strings.Fixed.Index (Hdr, "@");
      LP_Pos : constant Natural := (if At_Pos > 0 then Ada.Strings.Fixed.Index (Hdr, "(", From => At_Pos) else 0);
      FName  : constant String :=
        (if At_Pos > 0 and then LP_Pos > At_Pos then Hdr (At_Pos .. LP_Pos - 1) else "");
   begin
      if FName'Length = 0 or else Index (E.Def_Funcs, "|" & FName & "|") = 0 then
         if FName'Length > 0 then
            Append (E.Def_Funcs, FName & "|");
         end if;
         Append (E.Funcs_Buf, E.Cur.Header & " {" & NL);
         Append (E.Funcs_Buf, ".entry:" & NL);
         Append (E.Funcs_Buf, E.Cur.Entry_Alloc);
         Append (E.Funcs_Buf, E.Cur.Entry_Body);
         Append (E.Funcs_Buf, E.Cur.Rest_Body);
         Append (E.Funcs_Buf, "}" & NL & NL);
      end if;
      E.Cur := Saved;
   end End_Func;

   function Img (V : Long_Long_Integer) return String is
      S : constant String := Long_Long_Integer'Image (V);
   begin
      if S (S'First) = ' ' then
         return S (S'First + 1 .. S'Last);
      else
         return S;
      end if;
   end Img;

   function Img (V : Integer) return String is
      S : constant String := Integer'Image (V);
   begin
      if S (S'First) = ' ' then
         return S (S'First + 1 .. S'Last);
      else
         return S;
      end if;
   end Img;

   function Next_Reg (E : in out Emitter; Prefix : String := "t") return String is
      N : constant Natural := E.Cur.Reg_Counter;
   begin
      E.Cur.Reg_Counter := N + 1;
      return "%" & Prefix & "_" & Img (N);
   end Next_Reg;

   function Next_Label (E : in out Emitter; Prefix : String := "L") return String is
      N : constant Natural := E.Cur.Lbl_Counter;
   begin
      E.Cur.Lbl_Counter := N + 1;
      return "." & Prefix & Img (N);
   end Next_Label;

   function Next_Sym_Id (E : in out Emitter) return Natural is
      N : constant Natural := E.Sym_Counter;
   begin
      E.Sym_Counter := N + 1;
      return N;
   end Next_Sym_Id;

   procedure Emit_Entry_Alloca (E : in out Emitter; Line : String) is
   begin
      Append (E.Cur.Entry_Alloc, "    " & Line & NL);
   end Emit_Entry_Alloca;

   procedure Emit_Label (E : in out Emitter; Label : String; Params : String := "") is
   begin
      E.Cur.In_Entry := False;
      E.Cur.Terminated := False;
      if Params'Length > 0 then
         Append (E.Cur.Rest_Body, Label & "(" & Params & "):" & NL);
      else
         Append (E.Cur.Rest_Body, Label & ":" & NL);
      end if;
   end Emit_Label;

   procedure Emit_Inst (E : in out Emitter; Line : String) is
   begin
      if E.Cur.Terminated then
         return;
      end if;
      if E.Cur.In_Entry then
         Append (E.Cur.Entry_Body, "    " & Line & NL);
      else
         Append (E.Cur.Rest_Body, "    " & Line & NL);
      end if;
   end Emit_Inst;

   procedure Emit_Term (E : in out Emitter; Line : String) is
   begin
      if E.Cur.Terminated then
         return;
      end if;
      if E.Cur.In_Entry then
         Append (E.Cur.Entry_Body, "    " & Line & NL);
      else
         Append (E.Cur.Rest_Body, "    " & Line & NL);
      end if;
      E.Cur.Terminated := True;
   end Emit_Term;

   procedure Emit_Comment (E : in out Emitter; Text : String) is
   begin
      if E.Cur.In_Entry then
         Append (E.Cur.Entry_Body, "    // " & Text & NL);
      else
         Append (E.Cur.Rest_Body, "    // " & Text & NL);
      end if;
   end Emit_Comment;

   function Is_Terminated (E : Emitter) return Boolean is
   begin
      return E.Cur.Terminated;
   end Is_Terminated;

   procedure Set_Terminated (E : in out Emitter; Val : Boolean) is
   begin
      E.Cur.Terminated := Val;
   end Set_Terminated;

   function Escape_Pir_String (S : String) return String is
      Hex : constant String := "0123456789abcdef";
      Res : Unbounded_String;
   begin
      for C of S loop
         case C is
            when '"' => Append (Res, "\""");
            when '\' => Append (Res, "\\");
            when ASCII.LF => Append (Res, "\n");
            when ASCII.CR => Append (Res, "\r");
            when ASCII.HT => Append (Res, "\t");
            when ASCII.NUL => Append (Res, "\0");
            when others =>
               declare
                  Code : constant Natural := Character'Pos (C);
               begin
                  if Code >= 32 and then Code <= 126 then
                     Append (Res, C);
                  else
                     Append (Res, "\x" & Hex (Code / 16 + 1) & Hex (Code mod 16 + 1));
                  end if;
               end;
         end case;
      end loop;
      return To_String (Res);
   end Escape_Pir_String;

   procedure Write_Module (E : Emitter; Output_Path : String := "") is
      Out_File : Ada.Text_IO.File_Type;
      procedure Dump (F : Ada.Text_IO.File_Type) is
      begin
         Ada.Text_IO.Put_Line (F, "target """ & To_String (E.Target) & """");
         Ada.Text_IO.New_Line (F);
         if Length (E.Types_Buf) > 0 then
            Ada.Text_IO.Put_Line (F, To_String (E.Types_Buf));
         end if;
         if Length (E.Sigs_Buf) > 0 then
            Ada.Text_IO.Put_Line (F, To_String (E.Sigs_Buf));
         end if;
         if Length (E.Data_Buf) > 0 then
            Ada.Text_IO.Put_Line (F, To_String (E.Data_Buf));
         end if;
         if Length (E.Globals_Buf) > 0 then
            Ada.Text_IO.Put_Line (F, To_String (E.Globals_Buf));
         end if;
         if Length (E.Externs_Buf) > 0 then
            -- Filter out extern func declarations that were subsequently defined in this module
            declare
               Ext_S : constant String := To_String (E.Externs_Buf);
               Pos   : Positive := Ext_S'First;
            begin
               while Pos <= Ext_S'Last loop
                  declare
                     LF_Pos : constant Natural := Ada.Strings.Fixed.Index (Ext_S, (1 => NL), From => Pos);
                     End_P  : constant Natural := (if LF_Pos > 0 then LF_Pos - 1 else Ext_S'Last);
                     Line   : constant String := Ext_S (Pos .. End_P);
                     At_Pos : constant Natural := Ada.Strings.Fixed.Index (Line, "@");
                     LP_Pos : constant Natural := (if At_Pos > 0 then Ada.Strings.Fixed.Index (Line, "(", From => At_Pos) else 0);
                     Skip   : Boolean := False;
                  begin
                     if At_Pos > 0 and then LP_Pos > At_Pos then
                        declare
                           EName : constant String := Line (At_Pos .. LP_Pos - 1);
                        begin
                           if Index (E.Def_Funcs, "|" & EName & "|") > 0 then
                              Skip := True;
                           end if;
                        end;
                     end if;
                     if not Skip and then Line'Length > 0 then
                        Ada.Text_IO.Put_Line (F, Line);
                     end if;
                     if LF_Pos = 0 then
                        exit;
                     end if;
                     Pos := LF_Pos + 1;
                  end;
               end loop;
               Ada.Text_IO.New_Line (F);
            end;
         end if;
         Ada.Text_IO.Put (F, To_String (E.Funcs_Buf));
      end Dump;
   begin
      if Output_Path'Length = 0 then
         Dump (Ada.Text_IO.Standard_Output);
      else
         Ada.Text_IO.Create (Out_File, Ada.Text_IO.Out_File, Output_Path);
         Dump (Out_File);
         Ada.Text_IO.Close (Out_File);
      end if;
   end Write_Module;

end Pir_Emit;
