-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Hashed_Maps;
with Ada.Containers.Hashed_Sets;
with Ada.Containers.Vectors;
with Ada.Directories;
with Ada.Strings;
with Ada.Strings.Fixed;
with Ada.Strings.Hash;
with Ada.Strings.Maps.Constants;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with Interfaces; use Interfaces;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;

package body Lib_Binder is

   function To_Lower (S : String) return String is
   begin
      return Ada.Strings.Fixed.Translate (S, Ada.Strings.Maps.Constants.Lower_Case_Map);
   end To_Lower;

   function To_Upper (S : String) return String is
   begin
      return Ada.Strings.Fixed.Translate (S, Ada.Strings.Maps.Constants.Upper_Case_Map);
   end To_Upper;

   function Trim_Img (V : Integer) return String is
      S : constant String := Integer'Image (V);
   begin
      if S'Length > 0 and then S (S'First) = ' ' then
         return S (S'First + 1 .. S'Last);
      end if;
      return S;
   end Trim_Img;

   function Trim_Img64 (V : Long_Long_Integer) return String is
      S : constant String := Long_Long_Integer'Image (V);
   begin
      if S'Length > 0 and then S (S'First) = ' ' then
         return S (S'First + 1 .. S'Last);
      end if;
      return S;
   end Trim_Img64;

   function Lib_Pkg_Task_Group_Id (Name : String) return Long_Long_Integer is
      Up : constant String := To_Upper (Name);
      H  : Unsigned_32 := 16#811C_9DC5#;
   begin
      for C of Up loop
         H := H xor Unsigned_32 (Character'Pos (C));
         H := H * 16#0100_0193#;
      end loop;
      return Long_Long_Integer ((H and 16#3FFF_FFFF#) or 16#4000_0000#);
   end Lib_Pkg_Task_Group_Id;

   package Str_Vecs is new Ada.Containers.Vectors (Natural, Unbounded_String);

   function UHash (K : Unbounded_String) return Ada.Containers.Hash_Type is
   begin
      return Ada.Strings.Hash (To_String (K));
   end UHash;

   package USet is new Ada.Containers.Hashed_Sets
     (Element_Type        => Unbounded_String,
      Hash                => UHash,
      Equivalent_Elements => "=");

   function Is_Std_Unit (Name_Low : String) return Boolean is
   begin
      return Name_Low = "standard" or else Name_Low = "_standrd"
        or else Name_Low = "system" or else Name_Low = "text_io"
        or else Name_Low = "io_exceptions" or else Name_Low = "calendar"
        or else Name_Low = "sequential_io" or else Name_Low = "direct_io"
        or else Name_Low = "unchecked_conversion" or else Name_Low = "unchecked_deallocation"
        or else Name_Low = "machine_code" or else Name_Low = "low_level_io"
        or else Name_Low = "report" or else Name_Low = "spprt13"
        or else Name_Low = "fcndecl" or else Name_Low = "length_check"
        or else Name_Low = "enum_check" or else Name_Low = "check_file";
   end Is_Std_Unit;

   function Is_RTL_Unit (Name_Low : String; User_Has_Standard : Boolean) return Boolean is
   begin
      if Name_Low = "standard" then
         return not User_Has_Standard;
      end if;
      return Name_Low = "_standrd"
        or else Name_Low = "system" or else Name_Low = "text_io"
        or else Name_Low = "io_exceptions" or else Name_Low = "calendar"
        or else Name_Low = "sequential_io" or else Name_Low = "direct_io"
        or else Name_Low = "unchecked_conversion" or else Name_Low = "unchecked_deallocation"
        or else Name_Low = "machine_code" or else Name_Low = "low_level_io"
        or else Name_Low = "length_check" or else Name_Low = "enum_check"
        or else Name_Low = "check_file";
   end Is_RTL_Unit;

   function Is_Support_Pkg (Name_Low : String) return Boolean is
   begin
      return Name_Low = "report" or else Name_Low = "fcndecl" or else Name_Low = "spprt13";
   end Is_Support_Pkg;

   function Support_Pkg_Has_Body (Name_Low : String) return Boolean is
   begin
      return Name_Low = "report";
   end Support_Pkg_Has_Body;

   -- =========================================================================
   -- AST Inspection Helpers for Record_Compilation_Units
   -- =========================================================================

   function Parent_Chain_Str (N : TREE) return String is
   begin
      if Is_Void_Tree (N) then
         return "";
      end if;
      if KIND (N) = DN_SELECTED then
         return Parent_Chain_Str (D (AS_NAME, N)) & "." & To_Lower (Sym_Name (D (AS_DESIGNATOR, N)));
      else
         return To_Lower (Sym_Name (N));
      end if;
   end Parent_Chain_Str;

   function Decl_List_Requires_Body (Decl_S : TREE) return Boolean is
      L  : SEQ_TYPE;
      It : TREE;
   begin
      if Is_Void_Tree (Decl_S) then
         return False;
      end if;
      L := LIST (Decl_S);
      while not IS_EMPTY (L) loop
         POP (L, It);
         case KIND (It) is
            when DN_SUBPROG_ENTRY_DECL =>
               declare
                  UK  : constant TREE := D (AS_UNIT_KIND, It);
                  SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
               begin
                  if Is_Void_Tree (UK) then
                     if Is_Void_Tree (SID)
                       or else not (KIND (SID) in CLASS_SUBPROG_NAME
                                    and then not Is_Void_Tree (D (SM_INTERFACE, SID)))
                     then
                        return True;
                     end if;
                  end if;
               end;
            when DN_TASK_DECL | DN_GENERIC_DECL =>
               return True;
            when DN_PACKAGE_DECL =>
               declare
                  UK  : constant TREE := D (AS_UNIT_KIND, It);
                  Hdr : constant TREE := D (AS_HEADER, It);
               begin
                  if Is_Void_Tree (UK) and then not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                     if Decl_List_Requires_Body (D (AS_DECL_S1, Hdr))
                       or else Decl_List_Requires_Body (D (AS_DECL_S2, Hdr))
                     then
                        return True;
                     end if;
                  end if;
               end;
            when others =>
               null;
         end case;
      end loop;
      return False;
   end Decl_List_Requires_Body;

   function Decl_List_Has_Tasks_AST (Decl_S : TREE) return Boolean is
      function Type_Has_Task (Raw_T : TREE; Depth : Natural := 0) return Boolean is
         R : constant TREE := Resolve_Type_Spec (Raw_T);
      begin
         if Is_Void_Tree (R) or else Depth > 8 then
            return False;
         end if;
         if Is_Task_Type (R) then
            return True;
         elsif Is_Constrained_Array (R) or else KIND (R) = DN_ARRAY then
            declare
               BT : constant TREE := Base_Type_Spec (R);
            begin
               return not Is_Void_Tree (BT) and then KIND (BT) = DN_ARRAY
                 and then Type_Has_Task (D (SM_COMP_TYPE, BT), Depth + 1);
            end;
         elsif Is_Record_Type (R) then
            declare
               BT : constant TREE := Base_Type_Spec (R);
            begin
               if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
                  declare
                     CL : constant TREE := D (SM_COMP_LIST, BT);
                  begin
                     if not Is_Void_Tree (CL) and then not Is_Void_Tree (D (AS_DECL_S, CL)) then
                        declare
                           DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                           It : TREE;
                        begin
                           while not IS_EMPTY (DL) loop
                              POP (DL, It);
                              if KIND (It) = DN_VARIABLE_DECL then
                                 declare
                                    CID : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, It)));
                                 begin
                                    if Type_Has_Task (D (SM_OBJ_TYPE, CID), Depth + 1) then
                                       return True;
                                    end if;
                                 end;
                              end if;
                           end loop;
                        end;
                     end if;
                  end;
               end if;
            end;
         end if;
         return False;
      end Type_Has_Task;

      L  : SEQ_TYPE;
      It : TREE;
   begin
      if Is_Void_Tree (Decl_S) then
         return False;
      end if;
      L := LIST (Decl_S);
      while not IS_EMPTY (L) loop
         POP (L, It);
         case KIND (It) is
            when DN_TASK_DECL =>
               return True;
            when DN_VARIABLE_DECL =>
               declare
                  VID : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, It)));
               begin
                  if Type_Has_Task (D (SM_OBJ_TYPE, VID)) then
                     return True;
                  end if;
               end;
            when DN_PACKAGE_DECL =>
               declare
                  Hdr : constant TREE := D (AS_HEADER, It);
               begin
                  if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                     if Decl_List_Has_Tasks_AST (D (AS_DECL_S1, Hdr))
                       or else Decl_List_Has_Tasks_AST (D (AS_DECL_S2, Hdr))
                     then
                        return True;
                     end if;
                  end if;
               end;
            when others =>
               null;
         end case;
      end loop;
      return False;
   end Decl_List_Has_Tasks_AST;

   procedure Collect_Stubs_From_Block (Blk : TREE; Out_CSV : in out Unbounded_String) is
      L  : SEQ_TYPE;
      It : TREE;
   begin
      if Is_Void_Tree (Blk) or else KIND (Blk) /= DN_BLOCK_BODY then
         return;
      end if;
      if Is_Void_Tree (D (AS_ITEM_S, Blk)) then
         return;
      end if;
      L := LIST (D (AS_ITEM_S, Blk));
      while not IS_EMPTY (L) loop
         POP (L, It);
         if KIND (It) in DN_SUBPROGRAM_BODY | DN_PACKAGE_BODY | DN_TASK_BODY then
            declare
               B : constant TREE := D (AS_BODY, It);
            begin
               if not Is_Void_Tree (B) and then KIND (B) = DN_STUB then
                  declare
                     SN : constant String := To_Lower (Sym_Name (D (AS_SOURCE_NAME, It)));
                  begin
                     if SN'Length > 0 then
                        if Length (Out_CSV) > 0 then
                           Append (Out_CSV, ",");
                        end if;
                        Append (Out_CSV, SN);
                     end if;
                  end;
               end if;
            end;
         end if;
      end loop;
   end Collect_Stubs_From_Block;

   procedure Collect_Context_From_CU
     (CU        : TREE;
      Withs_CSV : out Unbounded_String;
      Elabs_CSV : out Unbounded_String)
   is
      procedure Scan_Ctx (Seq_N : TREE) is
         CL : SEQ_TYPE;
         CE : TREE;
      begin
         if Is_Void_Tree (Seq_N) then
            return;
         end if;
         CL := LIST (Seq_N);
         while not IS_EMPTY (CL) loop
            POP (CL, CE);
            if KIND (CE) = DN_WITH then
               declare
                  NL : SEQ_TYPE := LIST (D (AS_NAME_S, CE));
                  NM : TREE;
               begin
                  while not IS_EMPTY (NL) loop
                     POP (NL, NM);
                     declare
                        WN : constant String := To_Lower (Sym_Name (NM));
                     begin
                        if WN'Length > 0 then
                           if Length (Withs_CSV) > 0 then
                              Append (Withs_CSV, ",");
                           end if;
                           Append (Withs_CSV, WN);
                        end if;
                     end;
                  end loop;
               end;
               Scan_Ctx (D (AS_USE_PRAGMA_S, CE));
            elsif KIND (CE) = DN_PRAGMA then
               declare
                  PID_N  : constant TREE := D (AS_USED_NAME_ID, CE);
                  PN_Str : constant String := To_Upper (Sym_Name (PID_N));
               begin
                  -- Respect semphase validation: ignored pragmas have SM_DEFN = CONST_VOID
                  if PN_Str = "ELABORATE"
                    and then not Is_Void_Tree (PID_N)
                    and then not Is_Void_Tree (D (SM_DEFN, PID_N))
                    and then not Is_Void_Tree (D (AS_GENERAL_ASSOC_S, CE))
                  then
                     declare
                        AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, CE));
                        AI : TREE;
                     begin
                        while not IS_EMPTY (AL) loop
                           POP (AL, AI);
                           declare
                              Exp : constant TREE := (if KIND (AI) = DN_ASSOC then D (AS_EXP, AI) else AI);
                              EN  : constant String := To_Lower (Sym_Name (Exp));
                           begin
                              if EN'Length > 0 then
                                 if Length (Elabs_CSV) > 0 then
                                    Append (Elabs_CSV, ",");
                                 end if;
                                 Append (Elabs_CSV, EN);
                              end if;
                           end;
                        end loop;
                     end;
                  end if;
               end;
            end if;
         end loop;
      end Scan_Ctx;
   begin
      Withs_CSV := Null_Unbounded_String;
      Elabs_CSV := Null_Unbounded_String;
      -- Only scan AS_CONTEXT_ELEM_S (LRM 10.5(8): PRAGMA ELABORATE must be in context clause)
      Scan_Ctx (D (AS_CONTEXT_ELEM_S, CU));
   end Collect_Context_From_CU;

   function Read_Max_Seq (Idx_Path : String) return Natural is
      F    : Ada.Text_IO.File_Type;
      Line : String (1 .. 4096);
      Last : Natural;
      MaxS : Natural := 0;
   begin
      if not Ada.Directories.Exists (Idx_Path) then
         return 0;
      end if;
      Ada.Text_IO.Open (F, Ada.Text_IO.In_File, Idx_Path);
      while not Ada.Text_IO.End_Of_File (F) loop
         Ada.Text_IO.Get_Line (F, Line, Last);
         if Last >= 3 and then Line (1 .. 2) = "U|" then
            declare
               P2 : constant Natural := Ada.Strings.Fixed.Index (Line (3 .. Last), "|");
            begin
               if P2 > 3 then
                  declare
                     S : constant Natural := Natural'Value (Line (3 .. P2 - 1));
                  begin
                     if S > MaxS then
                        MaxS := S;
                     end if;
                  exception
                     when others => null;
                  end;
               end if;
            end;
         end if;
      end loop;
      Ada.Text_IO.Close (F);
      return MaxS;
   exception
      when others =>
         if Ada.Text_IO.Is_Open (F) then
            Ada.Text_IO.Close (F);
         end if;
         return MaxS;
   end Read_Max_Seq;

   procedure Record_Compilation_Units
     (Comp_Node : TREE;
      Obj_Path  : String;
      Lib_Dir   : String := "ADA__LIB")
   is
      Idx_Path : constant String := Lib_Dir & "/UNITS.IDX";
      Seq      : Natural := Read_Max_Seq (Idx_Path);
      F        : Ada.Text_IO.File_Type;
      CL       : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      CU       : TREE;
   begin
      if not Ada.Directories.Exists (Lib_Dir) then
         Ada.Directories.Create_Path (Lib_Dir);
      end if;
      if Ada.Directories.Exists (Idx_Path) then
         Ada.Text_IO.Open (F, Ada.Text_IO.Append_File, Idx_Path);
      else
         Ada.Text_IO.Create (F, Ada.Text_IO.Out_File, Idx_Path);
      end if;

      while not IS_EMPTY (CL) loop
         POP (CL, CU);
         declare
            Raw_AD       : constant TREE := D (AS_ALL_DECL, CU);
            Kind_Str     : Unbounded_String;
            UName        : Unbounded_String;
            Parent_Str   : Unbounded_String;
            Is_Gen       : Boolean := False;
            Req_Body     : Boolean := False;
            Is_Main_Cand : Boolean := False;
            Has_Tasks    : Boolean := False;
            Withs_CSV    : Unbounded_String;
            Elabs_CSV    : Unbounded_String;
            Stubs_CSV    : Unbounded_String;
            Valid        : Boolean := False;
         begin
            if not Is_Void_Tree (Raw_AD) then
               Collect_Context_From_CU (CU, Withs_CSV, Elabs_CSV);
               case KIND (Raw_AD) is
                  when DN_SUBUNIT =>
                     declare
                        SB : constant TREE := D (AS_SUBUNIT_BODY, Raw_AD);
                     begin
                        if not Is_Void_Tree (SB) then
                           Kind_Str   := To_Unbounded_String ("subunit");
                           Parent_Str := To_Unbounded_String (Parent_Chain_Str (D (AS_NAME, Raw_AD)));
                           UName      := To_Unbounded_String (To_Lower (Sym_Name (D (AS_SOURCE_NAME, SB))));
                           if KIND (SB) in DN_SUBPROGRAM_BODY | DN_PACKAGE_BODY | DN_TASK_BODY then
                              Collect_Stubs_From_Block (D (AS_BODY, SB), Stubs_CSV);
                           end if;
                           Valid := True;
                        end if;
                     end;

                  when DN_GENERIC_DECL =>
                     declare
                        Hdr : constant TREE := D (AS_HEADER, Raw_AD);
                        GID : constant TREE := D (AS_SOURCE_NAME, Raw_AD);
                     begin
                        UName  := To_Unbounded_String (To_Lower (Sym_Name (GID)));
                        Is_Gen := True;
                        if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                           Kind_Str := To_Unbounded_String ("package_spec");
                           Req_Body := Decl_List_Requires_Body (D (AS_DECL_S1, Hdr))
                             or else Decl_List_Requires_Body (D (AS_DECL_S2, Hdr));
                        elsif not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_FUNCTION_SPEC then
                           Kind_Str := To_Unbounded_String ("function_spec");
                           Req_Body := True;
                        else
                           Kind_Str := To_Unbounded_String ("procedure_spec");
                           Req_Body := True;
                        end if;
                        Valid := True;
                     end;

                  when DN_PACKAGE_DECL =>
                     declare
                        PID : constant TREE := D (AS_SOURCE_NAME, Raw_AD);
                        UK  : constant TREE := D (AS_UNIT_KIND, Raw_AD);
                        Hdr : constant TREE := D (AS_HEADER, Raw_AD);
                     begin
                        UName := To_Unbounded_String (To_Lower (Sym_Name (PID)));
                        if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                           Kind_Str := To_Unbounded_String ("package_inst");
                        elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                           Kind_Str := To_Unbounded_String ("package_ren");
                        else
                           Kind_Str := To_Unbounded_String ("package_spec");
                           if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                              Req_Body := Decl_List_Requires_Body (D (AS_DECL_S1, Hdr))
                                or else Decl_List_Requires_Body (D (AS_DECL_S2, Hdr));
                              Has_Tasks := Decl_List_Has_Tasks_AST (D (AS_DECL_S1, Hdr))
                                or else Decl_List_Has_Tasks_AST (D (AS_DECL_S2, Hdr));
                           end if;
                        end if;
                        Valid := True;
                     end;

                  when DN_PACKAGE_BODY =>
                     declare
                        PID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Raw_AD));
                     begin
                        UName    := To_Unbounded_String (To_Lower (Sym_Name (PID)));
                        Kind_Str := To_Unbounded_String ("package_body");
                        Is_Gen   := (KIND (PID) = DN_GENERIC_ID);
                        Collect_Stubs_From_Block (D (AS_BODY, Raw_AD), Stubs_CSV);
                        Valid    := True;
                     end;

                  when DN_SUBPROG_ENTRY_DECL =>
                     declare
                        SID : constant TREE := D (AS_SOURCE_NAME, Raw_AD);
                        UK  : constant TREE := D (AS_UNIT_KIND, Raw_AD);
                        Hdr : constant TREE := D (AS_HEADER, Raw_AD);
                        Is_Fn : constant Boolean :=
                          (not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_FUNCTION_SPEC)
                          or else KIND (Canonical_Def (SID)) = DN_FUNCTION_ID;
                     begin
                        UName := To_Unbounded_String (To_Lower (Sym_Name (SID)));
                        if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                           if Is_Fn then
                              Kind_Str := To_Unbounded_String ("function_inst");
                           else
                              Kind_Str := To_Unbounded_String ("procedure_inst");
                              declare
                                 Sp : constant TREE := D (SM_SPEC, Canonical_Def (SID));
                              begin
                                 if not Is_Void_Tree (Sp) and then KIND (Sp) = DN_PROCEDURE_SPEC
                                   and then (Is_Void_Tree (D (AS_PARAM_S, Sp))
                                             or else IS_EMPTY (LIST (D (AS_PARAM_S, Sp))))
                                 then
                                    Is_Main_Cand := True;
                                 end if;
                              end;
                           end if;
                        elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                           Kind_Str := To_Unbounded_String (if Is_Fn then "function_ren" else "procedure_ren");
                        else
                           Kind_Str := To_Unbounded_String (if Is_Fn then "function_spec" else "procedure_spec");
                           Req_Body := True;
                        end if;
                        Valid := True;
                     end;

                  when DN_SUBPROGRAM_BODY =>
                     declare
                        SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Raw_AD));
                        Hdr : constant TREE := D (AS_HEADER, Raw_AD);
                        Is_Fn : constant Boolean :=
                          not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_FUNCTION_SPEC;
                     begin
                        UName    := To_Unbounded_String (To_Lower (Sym_Name (SID)));
                        Kind_Str := To_Unbounded_String (if Is_Fn then "function_body" else "procedure_body");
                        Is_Gen   := (KIND (SID) = DN_GENERIC_ID);
                        if not Is_Gen and then not Is_Fn and then not Is_Void_Tree (Hdr)
                          and then KIND (Hdr) = DN_PROCEDURE_SPEC
                          and then (Is_Void_Tree (D (AS_PARAM_S, Hdr))
                                    or else IS_EMPTY (LIST (D (AS_PARAM_S, Hdr))))
                        then
                           Is_Main_Cand := True;
                        end if;
                        Collect_Stubs_From_Block (D (AS_BODY, Raw_AD), Stubs_CSV);
                        Valid := True;
                     end;

                  when others =>
                     null;
               end case;

               if Valid and then Length (UName) > 0 then
                  Seq := Seq + 1;
                  Ada.Text_IO.Put_Line (F,
                    "U|" & Trim_Img (Seq) &
                    "|" & To_String (Kind_Str) &
                    "|" & To_String (UName) &
                    "|" & To_String (Parent_Str) &
                    "|" & (if Is_Gen then "1" else "0") &
                    "|" & (if Req_Body then "1" else "0") &
                    "|" & (if Is_Main_Cand then "1" else "0") &
                    "|" & (if Has_Tasks then "1" else "0") &
                    "|" & Obj_Path &
                    "|" & To_String (Withs_CSV) &
                    "|" & To_String (Elabs_CSV) &
                    "|" & To_String (Stubs_CSV));
               end if;
            end if;
         end;
      end loop;

      Ada.Text_IO.Close (F);
   exception
      when others =>
         if Ada.Text_IO.Is_Open (F) then
            Ada.Text_IO.Close (F);
         end if;
         raise;
   end Record_Compilation_Units;

   -- =========================================================================
   -- Library Binder (LRM 10.3 & 10.5)
   -- =========================================================================

   type Unit_Rec is record
      Seq          : Natural := 0;
      Kind         : Unbounded_String;
      Name         : Unbounded_String;
      Parent       : Unbounded_String;
      Is_Gen       : Boolean := False;
      Req_Body     : Boolean := False;
      Is_Main_Cand : Boolean := False;
      Has_Tasks    : Boolean := False;
      Obj_Path     : Unbounded_String;
      Withs        : Str_Vecs.Vector;
      Elabs        : Str_Vecs.Vector;
      Stubs        : Str_Vecs.Vector;
   end record;

   package Unit_Vecs is new Ada.Containers.Vectors (Natural, Unit_Rec);
   package Unit_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unbounded_String,
      Element_Type    => Unit_Rec,
      Hash            => UHash,
      Equivalent_Keys => "=");
   package Seq_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unbounded_String,
      Element_Type    => Natural,
      Hash            => UHash,
      Equivalent_Keys => "=");

   procedure Split_CSV (S : String; V : in out Str_Vecs.Vector) is
      Pos : Positive := S'First;
   begin
      V.Clear;
      if S'Length = 0 then
         return;
      end if;
      while Pos <= S'Last loop
         declare
            C : constant Natural := Ada.Strings.Fixed.Index (S, ",", From => Pos);
            E : constant Natural := (if C > 0 then C - 1 else S'Last);
            T : constant String := Ada.Strings.Fixed.Trim (S (Pos .. E), Ada.Strings.Both);
         begin
            if T'Length > 0 then
               V.Append (To_Unbounded_String (T));
            end if;
            exit when C = 0;
            Pos := C + 1;
         end;
      end loop;
   end Split_CSV;

   function Unit_Supersede_Key (U : Unit_Rec) return String is
      K : constant String := To_String (U.Kind);
      N : constant String := To_String (U.Name);
   begin
      if K = "subunit" then
         return "sub:" & To_String (U.Parent) & ":" & N;
      elsif K = "package_body" then
         return "pb:" & N;
      elsif K = "package_spec" or else K = "package_inst" or else K = "package_ren" then
         return "ps:" & N;
      elsif K = "procedure_spec" or else K = "procedure_inst" or else K = "procedure_ren"
        or else K = "procedure_body"
      then
         return "subp:" & N;
      elsif K = "function_spec" or else K = "function_inst" or else K = "function_ren"
        or else K = "function_body"
      then
         return "subp:" & N;
      else
         return K & ":" & N;
      end if;
   end Unit_Supersede_Key;

   function Is_Spec_Kind (K : String) return Boolean is
   begin
      return K = "package_spec" or else K = "procedure_spec" or else K = "function_spec"
        or else K = "package_inst" or else K = "procedure_inst" or else K = "function_inst"
        or else K = "package_ren" or else K = "procedure_ren" or else K = "function_ren";
   end Is_Spec_Kind;

   function Is_Body_Kind (K : String) return Boolean is
   begin
      return K = "package_body" or else K = "procedure_body" or else K = "function_body";
   end Is_Body_Kind;

   function Root_Of_Parent (Parent : String) return String is
      Dot : constant Natural := Ada.Strings.Fixed.Index (Parent, ".");
   begin
      if Dot > 0 then
         return Parent (Parent'First .. Dot - 1);
      end if;
      return Parent;
   end Root_Of_Parent;

   function Ends_With_Unit (Parent : String; UName : String) return Boolean is
   begin
      if Parent = UName then
         return True;
      end if;
      if Parent'Length > UName'Length then
         declare
            Sfx : constant String := "." & UName;
         begin
            return Parent (Parent'Last - Sfx'Length + 1 .. Parent'Last) = Sfx;
         end;
      end if;
      return False;
   end Ends_With_Unit;

   procedure Bind_Library
     (Lib_Dir       : String;
      Main_Unit     : String;
      Target_Triple : String;
      Pir_Out_Path  : String;
      Objs_Out_Path : String;
      Check_Only    : Boolean;
      Skip_Checks   : Boolean;
      Success       : out Boolean;
      Main_Obj      : String := "")
   is
      Idx_Path    : constant String := Lib_Dir & "/UNITS.IDX";
      All_Units   : Unit_Vecs.Vector;
      Specs       : Unit_Maps.Map;
      Bodies      : Unit_Maps.Map;
      Subunits    : Unit_Maps.Map;
      Gen_Names   : USet.Set;
      Chosen_Main : Unbounded_String := To_Unbounded_String (To_Lower (Main_Unit));
      Spec_Time   : Seq_Maps.Map;

      procedure Fail (Msg : String) is
      begin
         Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, Msg);
         Success := False;
      end Fail;
   begin
      Success := False;
      if not Ada.Directories.Exists (Idx_Path) then
         Fail ("binder error: library index not found: " & Idx_Path);
         return;
      end if;

      -- 1. Load UNITS.IDX
      declare
         F    : Ada.Text_IO.File_Type;
         Line : String (1 .. 8192);
         Last : Natural;
      begin
         Ada.Text_IO.Open (F, Ada.Text_IO.In_File, Idx_Path);
         while not Ada.Text_IO.End_Of_File (F) loop
            Ada.Text_IO.Get_Line (F, Line, Last);
            if Last >= 3 and then Line (1 .. 2) = "U|" then
               declare
                  Fields : array (1 .. 13) of Unbounded_String;
                  FIdx   : Natural := 1;
                  Pos    : Positive := 1;
                  U      : Unit_Rec;
               begin
                  while Pos <= Last and then FIdx <= 13 loop
                     declare
                        Sep : constant Natural :=
                          (if FIdx < 13 then Ada.Strings.Fixed.Index (Line (Pos .. Last), "|") else 0);
                        End_P : constant Natural := (if Sep > 0 then Sep - 1 else Last);
                     begin
                        Fields (FIdx) := To_Unbounded_String (Line (Pos .. End_P));
                        FIdx := FIdx + 1;
                        exit when Sep = 0;
                        Pos := Sep + 1;
                     end;
                  end loop;
                  U.Seq          := Natural'Value (To_String (Fields (2)));
                  U.Kind         := Fields (3);
                  U.Name         := Fields (4);
                  U.Parent       := Fields (5);
                  U.Is_Gen       := (To_String (Fields (6)) = "1");
                  U.Req_Body     := (To_String (Fields (7)) = "1");
                  U.Is_Main_Cand := (To_String (Fields (8)) = "1");
                  U.Has_Tasks    := (To_String (Fields (9)) = "1");
                  U.Obj_Path     := Fields (10);
                  Split_CSV (To_String (Fields (11)), U.Withs);
                  Split_CSV (To_String (Fields (12)), U.Elabs);
                  Split_CSV (To_String (Fields (13)), U.Stubs);
                  All_Units.Append (U);

                  if U.Is_Gen then
                     Gen_Names.Include (U.Name);
                  end if;
                  if Length (Chosen_Main) = 0 and then U.Is_Main_Cand then
                     null; -- picked from latest below
                  end if;

                  declare
                     K : constant String := To_String (U.Kind);
                  begin
                     if K = "subunit" then
                        Subunits.Include (To_Unbounded_String (To_String (U.Parent) & "." & To_String (U.Name)), U);
                     elsif Is_Spec_Kind (K) then
                        Specs.Include (U.Name, U);
                     elsif Is_Body_Kind (K) then
                        -- LRM 10.1 (ca1020d): standalone subprogram body replaces generic subprogram instantiation
                        if (K = "procedure_body" or else K = "function_body")
                          and then Specs.Contains (U.Name)
                          and then (To_String (Specs.Element (U.Name).Kind) = "procedure_inst"
                                    or else To_String (Specs.Element (U.Name).Kind) = "function_inst")
                        then
                           Specs.Delete (U.Name);
                        end if;
                        Bodies.Include (U.Name, U);
                     end if;
                  end;
               end;
            end if;
         end loop;
         Ada.Text_IO.Close (F);
      end;

      -- Auto-select main procedure if not specified: prefer candidate from Main_Obj, else latest candidate
      if Length (Chosen_Main) = 0 and then Main_Obj'Length > 0 then
         for U of All_Units loop
            if U.Is_Main_Cand and then To_String (U.Obj_Path) = Main_Obj then
               Chosen_Main := U.Name;
            end if;
         end loop;
      end if;
      if Length (Chosen_Main) = 0 then
         for U of All_Units loop
            if U.Is_Main_Cand then
               Chosen_Main := U.Name;
            end if;
         end loop;
      end if;

      -- Build Spec_Time map (LRM 10.3(5))
      for C in Specs.Iterate loop
         Spec_Time.Include (Unit_Maps.Key (C), Unit_Maps.Element (C).Seq);
      end loop;
      for C in Bodies.Iterate loop
         declare
            UN : constant Unbounded_String := Unit_Maps.Key (C);
            BD : constant Unit_Rec := Unit_Maps.Element (C);
            K  : constant String := To_String (BD.Kind);
         begin
            if not Specs.Contains (UN) and then (K = "procedure_body" or else K = "function_body") then
               Spec_Time.Include (UN, BD.Seq);
            end if;
         end;
      end loop;

      -- 2. LRM 10.3 & 10.5 Validation
      if not Skip_Checks then
         -- 2a. Missing / obsolete required bodies
         for C in Specs.Iterate loop
            declare
               UN : constant String := To_String (Unit_Maps.Key (C));
               SP : constant Unit_Rec := Unit_Maps.Element (C);
            begin
               if not Is_Std_Unit (UN) and then SP.Req_Body then
                  if not Bodies.Contains (Unit_Maps.Key (C)) then
                     Fail ("binder error: missing body for library unit '" & UN & "'");
                     return;
                  elsif Bodies.Element (Unit_Maps.Key (C)).Seq < SP.Seq then
                     Fail ("binder error: obsolete body for recompiled unit '" & UN & "'");
                     return;
                  end if;
               end if;
            end;
         end loop;

         -- 2b. LRM 10.3(5): obsolete specs, required/subp bodies, and subunits due to recompiled withed spec
         for C in Specs.Iterate loop
            declare
               UN : constant String := To_String (Unit_Maps.Key (C));
               SP : constant Unit_Rec := Unit_Maps.Element (C);
            begin
               if not Is_Std_Unit (UN) then
                  for W of SP.Withs loop
                     if Spec_Time.Contains (W) and then Spec_Time.Element (W) > SP.Seq then
                        Fail ("binder error: unit '" & UN & "' is obsolete (withed unit '" & To_String (W) & "' was recompiled)");
                        return;
                     end if;
                  end loop;
               end if;
            end;
         end loop;

         for C in Bodies.Iterate loop
            declare
               UN : constant String := To_String (Unit_Maps.Key (C));
               BD : constant Unit_Rec := Unit_Maps.Element (C);
               Is_Optional_Pkg_Bdy : constant Boolean :=
                 To_String (BD.Kind) = "package_body"
                 and then Specs.Contains (Unit_Maps.Key (C))
                 and then not Specs.Element (Unit_Maps.Key (C)).Req_Body;
            begin
               -- Per LRM 10.3(5) / ACVC ca3006d, an obsolete OPTIONAL package body is simply dropped;
               -- all required bodies and standalone subprogram bodies must not be obsolete.
               if not Is_Std_Unit (UN) and then not Is_Optional_Pkg_Bdy then
                  for W of BD.Withs loop
                     if Spec_Time.Contains (W) and then Spec_Time.Element (W) > BD.Seq then
                        Fail ("binder error: unit '" & UN & "' is obsolete (withed unit '" & To_String (W) & "' was recompiled)");
                        return;
                     end if;
                  end loop;
               end if;
            end;
         end loop;

         for C in Subunits.Iterate loop
            declare
               SUB : constant Unit_Rec := Unit_Maps.Element (C);
            begin
               for W of SUB.Withs loop
                  if Spec_Time.Contains (W) and then Spec_Time.Element (W) > SUB.Seq then
                     Fail ("binder error: subunit '" & To_String (SUB.Parent) & "." & To_String (SUB.Name) &
                           "' is obsolete (withed unit '" & To_String (W) & "' was recompiled)");
                     return;
                  end if;
               end loop;
            end;
         end loop;

         -- 2c. Missing / obsolete separate subunits
         declare
            Err_Found : Boolean := False;
            procedure Check_Stubs_E (Info : Unit_Rec; Parent_Name : String) is
            begin
               if Err_Found then return; end if;
               for ST of Info.Stubs loop
                  declare
                     Found   : Boolean := False;
                     Sub_Seq : Natural := 0;
                     ST_Str  : constant String := To_String (ST);
                  begin
                     for SC in Subunits.Iterate loop
                        declare
                           SU : constant Unit_Rec := Unit_Maps.Element (SC);
                        begin
                           if To_String (SU.Name) = ST_Str
                             and then Ends_With_Unit (To_String (SU.Parent), Parent_Name)
                           then
                              Found := True;
                              Sub_Seq := SU.Seq;
                              exit;
                           end if;
                        end;
                     end loop;
                     if not Found then
                        Fail ("binder error: missing separate subunit '" & ST_Str & "' of '" & Parent_Name & "'");
                        Err_Found := True;
                        return;
                     end if;
                     if Sub_Seq < Info.Seq then
                        Fail ("binder error: obsolete separate subunit '" & ST_Str & "' of '" & Parent_Name & "'");
                        Err_Found := True;
                        return;
                     end if;
                  end;
               end loop;
            end Check_Stubs_E;
         begin
            for C in Specs.Iterate loop
               Check_Stubs_E (Unit_Maps.Element (C), To_String (Unit_Maps.Key (C)));
               exit when Err_Found;
            end loop;
            for C in Bodies.Iterate loop
               if not Err_Found then
                  Check_Stubs_E (Unit_Maps.Element (C), To_String (Unit_Maps.Key (C)));
               end if;
            end loop;
            for C in Subunits.Iterate loop
               if not Err_Found then
                  declare
                     SU : constant Unit_Rec := Unit_Maps.Element (C);
                  begin
                     Check_Stubs_E (SU, To_String (SU.Parent) & "." & To_String (SU.Name));
                  end;
               end if;
            end loop;
            if Err_Found then
               return;
            end if;
         end;

         -- 2d. LRM 10.5 circular elaboration dependency check (DFS 3-color over ("S",u) and ("B",u))
         declare
            type Node_Rec is record
               Key : Unbounded_String; -- "S:<name>" or "B:<name>"
               UName : Unbounded_String;
            end record;
            package Node_Vecs is new Ada.Containers.Vectors (Natural, Node_Rec);
            package Idx_Maps is new Ada.Containers.Hashed_Maps
              (Key_Type => Unbounded_String, Element_Type => Natural, Hash => UHash, Equivalent_Keys => "=");
            package Nat_Vecs is new Ada.Containers.Vectors (Natural, Natural);
            package Adj_Vecs is new Ada.Containers.Vectors (Natural, Nat_Vecs.Vector, "=" => Nat_Vecs."=");

            Nodes : Node_Vecs.Vector;
            NIdx  : Idx_Maps.Map;
            Adj   : Adj_Vecs.Vector;

            function Get_Node (Prefix : String; UN : String) return Natural is
               K : constant Unbounded_String := To_Unbounded_String (Prefix & ":" & UN);
               Empty_V : Nat_Vecs.Vector;
            begin
               if NIdx.Contains (K) then
                  return NIdx.Element (K);
               end if;
               Nodes.Append ((Key => K, UName => To_Unbounded_String (UN)));
               Adj.Append (Empty_V);
               NIdx.Insert (K, Natural (Nodes.Length) - 1);
               return Natural (Nodes.Length) - 1;
            end Get_Node;

            procedure Add_Edge (From_I, To_I : Natural) is
            begin
               if From_I /= To_I then
                  declare
                     V : Nat_Vecs.Vector := Adj.Element (From_I);
                  begin
                     V.Append (To_I);
                     Adj.Replace_Element (From_I, V);
                  end;
               end if;
            end Add_Edge;
         begin
            for C in Specs.Iterate loop
               declare
                  UN : constant String := To_String (Unit_Maps.Key (C));
                  SP : constant Unit_Rec := Unit_Maps.Element (C);
                  UI : Natural;
               begin
                  if not Is_Std_Unit (UN) then
                     UI := Get_Node ("S", UN);
                     for W of SP.Withs loop
                        if not Is_Std_Unit (To_String (W)) then
                           Add_Edge (UI, Get_Node ("S", To_String (W)));
                        end if;
                     end loop;
                     for E of SP.Elabs loop
                        if not Is_Std_Unit (To_String (E)) and then Bodies.Contains (E) then
                           Add_Edge (UI, Get_Node ("B", To_String (E)));
                        end if;
                     end loop;
                  end if;
               end;
            end loop;

            for C in Bodies.Iterate loop
               declare
                  UN : constant String := To_String (Unit_Maps.Key (C));
                  BD : constant Unit_Rec := Unit_Maps.Element (C);
                  UI : Natural;
               begin
                  if not Is_Std_Unit (UN) then
                     UI := Get_Node ("B", UN);
                     if Specs.Contains (Unit_Maps.Key (C)) then
                        Add_Edge (UI, Get_Node ("S", UN));
                     end if;
                     for W of BD.Withs loop
                        if not Is_Std_Unit (To_String (W)) then
                           Add_Edge (UI, Get_Node ("S", To_String (W)));
                        end if;
                     end loop;
                     for E of BD.Elabs loop
                        if not Is_Std_Unit (To_String (E)) and then Bodies.Contains (E) then
                           Add_Edge (UI, Get_Node ("B", To_String (E)));
                        end if;
                     end loop;
                  end if;
               end;
            end loop;

            for C in Subunits.Iterate loop
               declare
                  SU    : constant Unit_Rec := Unit_Maps.Element (C);
                  PRoot : constant String := Root_Of_Parent (To_String (SU.Parent));
                  UI    : Natural;
               begin
                  if not Is_Std_Unit (PRoot) and then Bodies.Contains (To_Unbounded_String (PRoot)) then
                     UI := Get_Node ("B", PRoot);
                     for W of SU.Withs loop
                        if not Is_Std_Unit (To_String (W)) then
                           Add_Edge (UI, Get_Node ("S", To_String (W)));
                        end if;
                     end loop;
                     for E of SU.Elabs loop
                        if not Is_Std_Unit (To_String (E)) and then Bodies.Contains (E) then
                           Add_Edge (UI, Get_Node ("B", To_String (E)));
                        end if;
                     end loop;
                  end if;
               end;
            end loop;

            declare
               N_Cnt : constant Natural := Natural (Nodes.Length);
               State : array (0 .. Natural'Max (0, N_Cnt)) of Natural := (others => 0);

               function Has_Cycle (U : Natural) return Boolean is
                  Nbrs : constant Nat_Vecs.Vector := Adj.Element (U);
               begin
                  State (U) := 1;
                  for V of Nbrs loop
                     if State (V) = 1 then
                        return True;
                     elsif State (V) = 0 and then Has_Cycle (V) then
                        return True;
                     end if;
                  end loop;
                  State (U) := 2;
                  return False;
               end Has_Cycle;
            begin
               for I in 0 .. N_Cnt - 1 loop
                  if State (I) = 0 and then Has_Cycle (I) then
                     Fail ("binder error: circular elaboration dependency involving '" &
                           To_String (Nodes.Element (I).UName) & "'");
                     return;
                  end if;
               end loop;
            end;
         end;
      end if;

      if Check_Only then
         Success := True;
         return;
      end if;

      -- 3. Select Live .o Files (LRM 10.3: drop superseded / obsoleted .o files)
      if Objs_Out_Path'Length > 0 then
         declare
            -- Determine latest seq for each supersede key
            Latest_Key : Seq_Maps.Map;
            -- Track units grouped by Obj_Path in compilation order
            Obj_Order  : Str_Vecs.Vector;
            Obj_Seen   : USet.Set;
            F_Out      : Ada.Text_IO.File_Type;
         begin
            for U of All_Units loop
               declare
                  SK : constant Unbounded_String := To_Unbounded_String (Unit_Supersede_Key (U));
               begin
                  Latest_Key.Include (SK, U.Seq);
               end;
               if Length (U.Obj_Path) > 0 and then not Obj_Seen.Contains (U.Obj_Path) then
                  Obj_Seen.Include (U.Obj_Path);
                  Obj_Order.Append (U.Obj_Path);
               end if;
            end loop;

            Ada.Text_IO.Create (F_Out, Ada.Text_IO.Out_File, Objs_Out_Path);
            for ObjP of Obj_Order loop
               declare
                  Has_Live_NonGen : Boolean := False;
               begin
                  for U of All_Units loop
                     if U.Obj_Path = ObjP then
                        declare
                           SK    : constant Unbounded_String := To_Unbounded_String (Unit_Supersede_Key (U));
                           Alive : Boolean := (Latest_Key.Element (SK) = U.Seq);
                           K     : constant String := To_String (U.Kind);
                        begin
                           -- Recompiling a spec/parent obsoletes prior bodies & subunits
                           if Alive and then K = "subunit" then
                              declare
                                 PRoot : constant Unbounded_String :=
                                   To_Unbounded_String (Root_Of_Parent (To_String (U.Parent)));
                              begin
                                 if Spec_Time.Contains (PRoot) and then Spec_Time.Element (PRoot) > U.Seq then
                                    Alive := False;
                                 end if;
                              end;
                           elsif Alive and then Is_Body_Kind (K) then
                              if Specs.Contains (U.Name) and then Specs.Element (U.Name).Seq > U.Seq then
                                 Alive := False;
                              end if;
                           end if;

                           if Alive and then not U.Is_Gen and then not Gen_Names.Contains (U.Name) then
                              Has_Live_NonGen := True;
                              exit;
                           end if;
                        end;
                     end if;
                  end loop;
                  if Has_Live_NonGen then
                     Ada.Text_IO.Put_Line (F_Out, To_String (ObjP));
                  end if;
               end;
            end loop;
            Ada.Text_IO.Close (F_Out);
         end;
      end if;

      -- 4. Emit binder_main.pir with LRM 10.5 topological elaboration order
      if Pir_Out_Path'Length > 0 then
         if Length (Chosen_Main) = 0 then
            Fail ("binder error: no parameterless main procedure found in library");
            return;
         end if;
         declare
            User_Has_Standard : constant Boolean := Specs.Contains (To_Unbounded_String ("standard"));
            Pkg_Specs         : USet.Set;
            Pkg_Bodies        : USet.Set;
            All_Withs         : USet.Set;

            type ENode is record
               Is_Body : Boolean := False;
               Name    : Unbounded_String;
            end record;
            package ENode_Vecs is new Ada.Containers.Vectors (Natural, ENode);
            ENodes : ENode_Vecs.Vector;
            EN_Idx : Seq_Maps.Map;

            function EKey (Is_B : Boolean; N : String) return Unbounded_String is
            begin
               return To_Unbounded_String ((if Is_B then "B:" else "S:") & N);
            end EKey;
         begin
            for C in Specs.Iterate loop
               declare
                  UN : constant String := To_String (Unit_Maps.Key (C));
                  SP : constant Unit_Rec := Unit_Maps.Element (C);
                  K  : constant String := To_String (SP.Kind);
               begin
                  if not Is_RTL_Unit (UN, User_Has_Standard) then
                     if (K = "package_spec" or else K = "package_inst"
                         or else K = "procedure_inst" or else K = "function_inst")
                       and then not SP.Is_Gen
                     then
                        Pkg_Specs.Include (Unit_Maps.Key (C));
                     end if;
                  end if;
                  for W of SP.Withs loop
                     All_Withs.Include (W);
                  end loop;
               end;
            end loop;

            for C in Bodies.Iterate loop
               declare
                  UN : constant String := To_String (Unit_Maps.Key (C));
                  BD : constant Unit_Rec := Unit_Maps.Element (C);
               begin
                  if not Is_RTL_Unit (UN, User_Has_Standard)
                    and then To_String (BD.Kind) = "package_body"
                    and then Pkg_Specs.Contains (Unit_Maps.Key (C))
                    and then Specs.Contains (Unit_Maps.Key (C))
                    and then BD.Seq > Specs.Element (Unit_Maps.Key (C)).Seq
                  then
                     -- LRM 10.3(5) (ca3006d): skip optional body obsoleted by recompiled withed spec
                     declare
                        Obs : Boolean := False;
                     begin
                        for W of BD.Withs loop
                           if Spec_Time.Contains (W) and then Spec_Time.Element (W) > BD.Seq then
                              Obs := True;
                              exit;
                           end if;
                        end loop;
                        if not Obs then
                           Pkg_Bodies.Include (Unit_Maps.Key (C));
                        end if;
                     end;
                  end if;
                  for W of BD.Withs loop
                     All_Withs.Include (W);
                  end loop;
               end;
            end loop;

            for C in Subunits.Iterate loop
               for W of Unit_Maps.Element (C).Withs loop
                  All_Withs.Include (W);
               end loop;
            end loop;

            -- Include support packages (REPORT, FCNDECL, SPPRT13) if withed
            declare
               Sup_Arr : constant array (1 .. 3) of Unbounded_String :=
                 (To_Unbounded_String ("report"),
                  To_Unbounded_String ("fcndecl"),
                  To_Unbounded_String ("spprt13"));
            begin
               for Sup of Sup_Arr loop
                  if All_Withs.Contains (Sup) then
                     Pkg_Specs.Include (Sup);
                     if Support_Pkg_Has_Body (To_String (Sup)) then
                        Pkg_Bodies.Include (Sup);
                     end if;
                  end if;
               end loop;
            end;

            -- Collect sorted node list
            declare
               Sorted_Pkgs : Str_Vecs.Vector;
            begin
               for P of Pkg_Specs loop
                  Sorted_Pkgs.Append (P);
               end loop;
               -- Insertion sort alphabetically for deterministic ordering
               for I in 1 .. Natural (Sorted_Pkgs.Length) - 1 loop
                  declare
                     KeyV : constant Unbounded_String := Sorted_Pkgs.Element (I);
                     J    : Integer := I - 1;
                  begin
                     while J >= 0 and then To_String (Sorted_Pkgs.Element (J)) > To_String (KeyV) loop
                        Sorted_Pkgs.Replace_Element (J + 1, Sorted_Pkgs.Element (J));
                        J := J - 1;
                     end loop;
                     Sorted_Pkgs.Replace_Element (J + 1, KeyV);
                  end;
               end loop;

               for P of Sorted_Pkgs loop
                  EN_Idx.Insert (EKey (False, To_String (P)), Natural (ENodes.Length));
                  ENodes.Append ((Is_Body => False, Name => P));
                  if Pkg_Bodies.Contains (P) then
                     EN_Idx.Insert (EKey (True, To_String (P)), Natural (ENodes.Length));
                     ENodes.Append ((Is_Body => True, Name => P));
                  end if;
               end loop;
            end;

            declare
               N : constant Natural := Natural (ENodes.Length);
               -- Prereq(U, V) = True means V must run BEFORE U
               type Bool_Mat is array (Natural range <>, Natural range <>) of Boolean;
               Prereq : Bool_Mat (0 .. Natural'Max (0, N), 0 .. Natural'Max (0, N)) :=
                 (others => (others => False));
               Done   : array (0 .. Natural'Max (0, N)) of Boolean := (others => False);
               Order  : array (1 .. Natural'Max (1, N)) of Natural := (others => 0);
               O_Cnt  : Natural := 0;

               procedure Add_Prereq (U_Key, V_Key : Unbounded_String) is
               begin
                  if EN_Idx.Contains (U_Key) and then EN_Idx.Contains (V_Key) then
                     declare
                        UI : constant Natural := EN_Idx.Element (U_Key);
                        VI : constant Natural := EN_Idx.Element (V_Key);
                     begin
                        if UI /= VI then
                           Prereq (UI, VI) := True;
                        end if;
                     end;
                  end if;
               end Add_Prereq;

               function Prio_Less (A, B : Natural) return Boolean is
                  NA : constant ENode := ENodes.Element (A);
                  NB : constant ENode := ENodes.Element (B);
                  SA : constant String := To_String (NA.Name);
                  SB : constant String := To_String (NB.Name);
                  RA : constant Natural :=
                    (if SA = "report" then 0 elsif Is_Support_Pkg (SA) then 1 else 2);
                  RB : constant Natural :=
                    (if SB = "report" then 0 elsif Is_Support_Pkg (SB) then 1 else 2);
               begin
                  if RA /= RB then return RA < RB; end if;
                  if NA.Is_Body /= NB.Is_Body then return not NA.Is_Body; end if;
                  return SA < SB;
               end Prio_Less;
            begin
               for P of Pkg_Bodies loop
                  Add_Prereq (EKey (True, To_String (P)), EKey (False, To_String (P)));
               end loop;

               for C in Specs.Iterate loop
                  declare
                     UN : constant String := To_String (Unit_Maps.Key (C));
                     SP : constant Unit_Rec := Unit_Maps.Element (C);
                     UK : constant Unbounded_String := EKey (False, UN);
                  begin
                     for W of SP.Withs loop
                        Add_Prereq (UK, EKey (False, To_String (W)));
                     end loop;
                     for E of SP.Elabs loop
                        Add_Prereq (UK, EKey (False, To_String (E)));
                        Add_Prereq (UK, EKey (True, To_String (E)));
                     end loop;
                  end;
               end loop;

               for C in Bodies.Iterate loop
                  declare
                     UN : constant String := To_String (Unit_Maps.Key (C));
                     BD : constant Unit_Rec := Unit_Maps.Element (C);
                     UK : constant Unbounded_String := EKey (True, UN);
                  begin
                     for W of BD.Withs loop
                        Add_Prereq (UK, EKey (False, To_String (W)));
                     end loop;
                     for E of BD.Elabs loop
                        Add_Prereq (UK, EKey (False, To_String (E)));
                        Add_Prereq (UK, EKey (True, To_String (E)));
                     end loop;
                  end;
               end loop;

               for C in Subunits.Iterate loop
                  declare
                     SU    : constant Unit_Rec := Unit_Maps.Element (C);
                     PRoot : constant String := Root_Of_Parent (To_String (SU.Parent));
                     UK    : constant Unbounded_String := EKey (True, PRoot);
                  begin
                     for W of SU.Withs loop
                        Add_Prereq (UK, EKey (False, To_String (W)));
                     end loop;
                     for E of SU.Elabs loop
                        Add_Prereq (UK, EKey (False, To_String (E)));
                        Add_Prereq (UK, EKey (True, To_String (E)));
                     end loop;
                  end;
               end loop;

               -- Support packages elaborate before user packages when unconstrained
               for P of Pkg_Specs loop
                  declare
                     PN : constant String := To_String (P);
                  begin
                     if not Is_Support_Pkg (PN) then
                        for Sup_I in 1 .. 3 loop
                           declare
                              SN : constant String :=
                                (case Sup_I is
                                   when 1 => "report",
                                   when 2 => "fcndecl",
                                   when 3 => "spprt13");
                           begin
                              Add_Prereq (EKey (False, PN), EKey (False, SN));
                              Add_Prereq (EKey (False, PN), EKey (True, SN));
                           end;
                        end loop;
                     end if;
                  end;
               end loop;

               -- Deterministic Kahn's topological sort
               while O_Cnt < N loop
                  declare
                     Pick : Integer := -1;
                  begin
                     for I in 0 .. N - 1 loop
                        if not Done (I) then
                           declare
                              Ready : Boolean := True;
                           begin
                              for J in 0 .. N - 1 loop
                                 if Prereq (I, J) and then not Done (J) then
                                    Ready := False;
                                    exit;
                                 end if;
                              end loop;
                              if Ready then
                                 if Pick < 0 or else Prio_Less (I, Natural (Pick)) then
                                    Pick := I;
                                 end if;
                              end if;
                           end;
                        end if;
                     end loop;
                     if Pick < 0 then
                        for I in 0 .. N - 1 loop
                           if not Done (I) then
                              Pick := I;
                              exit;
                           end if;
                        end loop;
                     end if;
                     exit when Pick < 0;
                     Done (Natural (Pick)) := True;
                     O_Cnt := O_Cnt + 1;
                     Order (O_Cnt) := Natural (Pick);
                  end;
               end loop;

               -- Emit binder_main.pir
               declare
                  PF : Ada.Text_IO.File_Type;
                  Main_Low : constant String := To_String (Chosen_Main);
                  Step_No  : Natural := 0;
               begin
                  Ada.Text_IO.Create (PF, Ada.Text_IO.Out_File, Pir_Out_Path);
                  Ada.Text_IO.Put_Line (PF, "target """ & Target_Triple & """");
                  Ada.Text_IO.Put_Line (PF, "extern func @__ada_global_master() -> ptr cc ""c""");
                  Ada.Text_IO.Put_Line (PF, "extern func @__ada_master_activate(%m: ptr) -> void cc ""c""");
                  Ada.Text_IO.Put_Line (PF, "extern func @__ada_master_activate_group(%m: ptr, %gid: i64) -> void cc ""c""");
                  Ada.Text_IO.Put_Line (PF, "extern func @__ada_master_leave(%m: ptr) -> void cc ""c""");
                  Ada.Text_IO.Put_Line (PF, "extern func @__ada_unhandled_exception(%e: ptr) -> void cc ""c""");
                  Ada.Text_IO.Put_Line (PF, "extern func @ada__" & Main_Low & "() -> void cc ""internal""");

                  for K in 1 .. O_Cnt loop
                     declare
                        EN  : constant ENode := ENodes.Element (Order (K));
                        Sfx : constant String := (if EN.Is_Body then "elab_body" else "elab_spec");
                        Sym : constant String := "@ada__" & To_String (EN.Name) & "__" & Sfx;
                     begin
                        Ada.Text_IO.Put_Line (PF, "extern func " & Sym & "() -> void cc ""internal""");
                     end;
                  end loop;

                  Ada.Text_IO.Put_Line (PF, "export func @main() -> i32 cc ""c"" {");
                  Ada.Text_IO.Put_Line (PF, ".entry:");
                  Ada.Text_IO.Put_Line (PF, "  %__gm = call @__ada_global_master()");

                  for K in 1 .. O_Cnt loop
                     declare
                        EN  : constant ENode := ENodes.Element (Order (K));
                        Sfx : constant String := (if EN.Is_Body then "elab_body" else "elab_spec");
                        Sym : constant String := "@ada__" & To_String (EN.Name) & "__" & Sfx;
                     begin
                        Step_No := Step_No + 1;
                        Ada.Text_IO.Put_Line (PF, "  invoke " & Sym & "() then .el_" & Trim_Img (Step_No) & " unwind .unhandled(exn)");
                        Ada.Text_IO.Put_Line (PF, ".el_" & Trim_Img (Step_No) & ":");
                        -- LRM 9.3(2): for a bodyless library package spec declaring tasks, activate its group right after elab_spec!
                        if not EN.Is_Body and then not Pkg_Bodies.Contains (EN.Name)
                          and then Specs.Contains (EN.Name) and then Specs.Element (EN.Name).Has_Tasks
                        then
                           Step_No := Step_No + 1;
                           Ada.Text_IO.Put_Line (PF,
                             "  invoke @__ada_master_activate_group(%__gm, " &
                             Trim_Img64 (Lib_Pkg_Task_Group_Id (To_String (EN.Name))) &
                             ") then .el_" & Trim_Img (Step_No) & " unwind .unhandled(exn)");
                           Ada.Text_IO.Put_Line (PF, ".el_" & Trim_Img (Step_No) & ":");
                        end if;
                     end;
                  end loop;

                  Ada.Text_IO.Put_Line (PF, "  invoke @__ada_master_activate(%__gm) then .run_main unwind .unhandled(exn)");
                  Ada.Text_IO.Put_Line (PF, ".run_main:");
                  Ada.Text_IO.Put_Line (PF, "  invoke @ada__" & Main_Low & "() then .done unwind .unhandled(exn)");
                  Ada.Text_IO.Put_Line (PF, ".done:");
                  Ada.Text_IO.Put_Line (PF, "  call @__ada_master_leave(%__gm)");
                  Ada.Text_IO.Put_Line (PF, "  ret 0");
                  Ada.Text_IO.Put_Line (PF, ".unhandled(%exn: ptr):");
                  Ada.Text_IO.Put_Line (PF, "  call @__ada_unhandled_exception(%exn)");
                  Ada.Text_IO.Put_Line (PF, "  unreachable");
                  Ada.Text_IO.Put_Line (PF, "}");
                  Ada.Text_IO.Close (PF);
               end;
            end;
         end;
      end if;

      Success := True;
   end Bind_Library;

end Lib_Binder;
