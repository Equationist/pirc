-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Fixed;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;

package body Cg_State is

   function Hash_U32 (K : Unsigned_32) return Ada.Containers.Hash_Type is
   begin
      return Ada.Containers.Hash_Type (K);
   end Hash_U32;

   function Align_Up (Val, Al : Natural) return Natural is
   begin
      if Al <= 1 then
         return Val;
      end if;
      return ((Val + Al - 1) / Al) * Al;
   end Align_Up;

   procedure Init_Context
     (Ctx           : in out Context;
      Target_Triple : String;
      Enable_Checks : Boolean;
      Emit_Main     : Boolean)
   is
   begin
      Pir_Emit.Init (Ctx.E, Target_Triple);
      Ctx.Enable_Checks := Enable_Checks;
      Ctx.Emit_Main     := Emit_Main;
      Ctx.Elab_Names    := Null_Unbounded_String;
   end Init_Context;

   function Subst_Tree (Ctx : Context; T : TREE) return TREE is
      Cur : TREE := T;
   begin
      if Is_Void_Tree (Cur) or else Ctx.Generic_Subst.Is_Empty then
         return Cur;
      end if;
      for Step in 1 .. 8 loop
         declare
            K : constant Unsigned_32 := Tree_Key (Cur);
         begin
            if Ctx.Generic_Subst.Contains (K) then
               Cur := Ctx.Generic_Subst.Element (K);
            else
               exit;
            end if;
         end;
      end loop;
      return Cur;
   end Subst_Tree;

   procedure Ensure_Runtime_Externs (Ctx : in out Context) is
   begin
      Add_Extern_Func (Ctx.E, "@__ada_raise",
        "extern func @__ada_raise(%tag: ptr) -> void cc ""c"" noreturn");
      Add_Extern_Func (Ctx.E, "@__ada_unhandled_exception",
        "extern func @__ada_unhandled_exception(%tag: ptr) -> void cc ""c"" noreturn");
      Add_Extern_Func (Ctx.E, "@__ada_put_char",
        "extern func @__ada_put_char(%c: i8) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_put_str",
        "extern func @__ada_put_str(%p: ptr, %n: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_put_line",
        "extern func @__ada_put_line(%p: ptr, %n: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_new_line",
        "extern func @__ada_new_line(%s: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_put_i64",
        "extern func @__ada_put_i64(%v: i64, %w: i32, %b: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_put_f64",
        "extern func @__ada_put_f64(%v: f64, %f: i32, %a: i32, %e: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_clock_ns",
        "extern func @__ada_clock_ns() -> i64 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_ipow_i32",
        "extern func @__ada_ipow_i32(%b: i32, %e: i32, %chk: i32) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_ipow_i64",
        "extern func @__ada_ipow_i64(%b: i64, %e: i32, %chk: i32) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_fpow_f32",
        "extern func @__ada_fpow_f32(%b: f32, %e: i32) -> f32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_fpow_f64",
        "extern func @__ada_fpow_f64(%b: f64, %e: i32) -> f64 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_alloc",
        "extern func @__ada_alloc(%sz: i64) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_free",
        "extern func @__ada_free(%p: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_ss_mark",
        "extern func @__ada_ss_mark() -> i64 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_ss_alloc",
        "extern func @__ada_ss_alloc(%sz: i64, %al: i64) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_ss_release",
        "extern func @__ada_ss_release(%mark: i64) -> void cc ""c"" nounwind");
   end Ensure_Runtime_Externs;

   function Get_Arr_Desc_Type (Ctx : in out Context; NDims : Natural) return String is
      ND   : constant Natural := (if NDims < 1 then 1 elsif NDims > 4 then 4 else NDims);
      Name : constant String := "@__ada_arr_desc_" & Img (ND);
      Decl : Unbounded_String := To_Unbounded_String ("type " & Name & " = { ptr");
   begin
      for I in 1 .. ND loop
         Append (Decl, ", i32, i32");
      end loop;
      Append (Decl, " }");
      Add_Type_Decl (Ctx.E, To_String (Decl));
      return Name;
   end Get_Arr_Desc_Type;

   procedure Ensure_Std_Exc_Extern (Ctx : in out Context; Sym : String) is
   begin
      Add_Global_Decl (Ctx.E, Sym, "extern " & Sym & " = data i8, align 1");
   end Ensure_Std_Exc_Extern;

   function Constraint_Error_Tag (Ctx : in out Context) return String is
   begin
      Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_constraint_error");
      return "@__ada_exc_constraint_error";
   end Constraint_Error_Tag;

   function Program_Error_Tag (Ctx : in out Context) return String is
   begin
      Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_program_error");
      return "@__ada_exc_program_error";
   end Program_Error_Tag;

   function Storage_Error_Tag (Ctx : in out Context) return String is
   begin
      Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_storage_error");
      return "@__ada_exc_storage_error";
   end Storage_Error_Tag;

   function Get_Exc_Tag (Ctx : in out Context; Exc_Id : TREE) return String is
      Can  : constant TREE := Canonical_Exc (Subst_Tree (Ctx, Exc_Id));
      Key  : constant Unsigned_32 := Tree_Key (Can);
      Name : constant String := Sym_Name (Can);
      Pkg  : constant String := Enclosing_Package_Name (Can);
   begin
      if Is_Void_Tree (Can) then
         return Constraint_Error_Tag (Ctx);
      end if;
      -- Standard exceptions (note AI83-00387 aliases NUMERIC_ERROR to CONSTRAINT_ERROR like GNAT)
      if Pkg = "STANDARD" or else Pkg = "" then
         if Name = "CONSTRAINT_ERROR" or else Name = "NUMERIC_ERROR" then
            return Constraint_Error_Tag (Ctx);
         elsif Name = "PROGRAM_ERROR" then
            return Program_Error_Tag (Ctx);
         elsif Name = "STORAGE_ERROR" then
            return Storage_Error_Tag (Ctx);
         elsif Name = "TASKING_ERROR" then
            Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_tasking_error");
            return "@__ada_exc_tasking_error";
         end if;
      elsif Pkg = "IO_EXCEPTIONS" then
         declare
            S : constant String := "@__ada_exc_" & Lower_Name (Name);
         begin
            Ensure_Std_Exc_Extern (Ctx, S);
            return S;
         end;
      elsif Pkg = "CALENDAR" and then Name = "TIME_ERROR" then
         Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_time_error");
         return "@__ada_exc_time_error";
      end if;

      if Ctx.Exc_Tags.Contains (Key) then
         return To_String (Ctx.Exc_Tags.Element (Key).Pir_Type);
      end if;

      declare
         QName : constant String := Qualified_Ada_Name (Can);
         Sym   : constant String := "@__ada_exc_" & QName & "_" & Tree_Key_Str (Can);
         TI    : Type_Info;
      begin
         Add_Data_Decl (Ctx.E, Sym,
           Sym & " = data align 1 { """ & Escape_Pir_String (Name) & "\0"" }");
         TI.Pir_Type := To_Unbounded_String (Sym);
         Ctx.Exc_Tags.Insert (Key, TI);
         return Sym;
      end;
   end Get_Exc_Tag;

   -- Build PIR struct type for a record (including discriminants and optional variant part)
   function Build_Record_Type (Ctx : in out Context; Rec_Spec : TREE) return Type_Info is
      Base_Rec : constant TREE := Base_Type_Spec (Rec_Spec);
      Key      : constant Unsigned_32 := Tree_Key (Base_Rec);
      TName    : constant String := "@ada_rec_" & Tree_Key_Str (Base_Rec);
      Info     : Type_Info;
      Fields_S : Unbounded_String;
      FIdx     : Natural := 0;
      Offset   : Natural := 0;
      Max_Al   : Natural := 1;

      procedure Append_Field (Comp_Id : TREE; FT : Type_Info) is
         FE : Field_Entry;
      begin
         if FIdx > 0 then
            Append (Fields_S, ", ");
         end if;
         Append (Fields_S, To_String (FT.Pir_Type));
         Offset := Align_Up (Offset, FT.Align) + FT.Byte_Size;
         if FT.Align > Max_Al then
            Max_Al := FT.Align;
         end if;
         FE.Field_Idx := FIdx;
         FE.In_Union  := False;
         Ctx.Fields.Include (Tree_Key (Comp_Id), FE);
         FIdx := FIdx + 1;
      end Append_Field;

      procedure Walk_Comp_List_Fixed (CL : TREE) is
         Decls : SEQ_TYPE;
         Decl  : TREE;
      begin
         if Is_Void_Tree (CL) then
            return;
         end if;
         declare
            DS : constant TREE := D (AS_DECL_S, CL);
         begin
            if not Is_Void_Tree (DS) then
               Decls := LIST (DS);
               while not IS_EMPTY (Decls) loop
                  POP (Decls, Decl);
                  if KIND (Decl) = DN_VARIABLE_DECL then
                     declare
                        Names : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Decl));
                        CID   : TREE;
                     begin
                        while not IS_EMPTY (Names) loop
                           POP (Names, CID);
                           declare
                              CT : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                              FT : constant Type_Info := Get_Type_Info (Ctx, CT);
                           begin
                              Append_Field (CID, FT);
                           end;
                        end loop;
                     end;
                  end if;
               end loop;
            end if;
         end;

         -- Variant part -> PIR union of variant structs
         declare
            VP : constant TREE := D (AS_VARIANT_PART, CL);
         begin
            if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART then
               declare
                  VS        : constant TREE := D (AS_VARIANT_S, VP);
                  VList     : SEQ_TYPE := LIST (VS);
                  VNode     : TREE;
                  Union_Sym : constant String := TName & "_u" & Img (FIdx);
                  U_Fields  : Unbounded_String;
                  U_Max_Sz  : Natural := 1;
                  U_Max_Al  : Natural := 1;
                  VCount    : Natural := 0;
                  Union_Pos : constant Natural := FIdx;
               begin
                  while not IS_EMPTY (VList) loop
                     POP (VList, VNode);
                     if KIND (VNode) = DN_VARIANT then
                        declare
                           VCL     : constant TREE := D (AS_COMP_LIST, VNode);
                           VSym    : constant String := Union_Sym & "_v" & Img (VCount);
                           VF_Str  : Unbounded_String;
                           VF_Idx  : Natural := 0;
                           V_Off   : Natural := 0;
                           V_Al    : Natural := 1;
                        begin
                           if not Is_Void_Tree (VCL) then
                              declare
                                 VDS   : constant TREE := D (AS_DECL_S, VCL);
                                 VDL   : SEQ_TYPE;
                                 VDecl : TREE;
                              begin
                                 if not Is_Void_Tree (VDS) then
                                    VDL := LIST (VDS);
                                    while not IS_EMPTY (VDL) loop
                                       POP (VDL, VDecl);
                                       if KIND (VDecl) = DN_VARIABLE_DECL then
                                          declare
                                             VNames : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, VDecl));
                                             VCID   : TREE;
                                          begin
                                             while not IS_EMPTY (VNames) loop
                                                POP (VNames, VCID);
                                                declare
                                                   CT : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, VCID));
                                                   FT : constant Type_Info := Get_Type_Info (Ctx, CT);
                                                   FE : Field_Entry;
                                                begin
                                                   if VF_Idx > 0 then
                                                      Append (VF_Str, ", ");
                                                   end if;
                                                   Append (VF_Str, To_String (FT.Pir_Type));
                                                   V_Off := Align_Up (V_Off, FT.Align) + FT.Byte_Size;
                                                   if FT.Align > V_Al then V_Al := FT.Align; end if;
                                                   FE.Field_Idx := Union_Pos;
                                                   FE.In_Union  := True;
                                                   FE.Union_Idx := Union_Pos;
                                                   FE.Var_Idx   := VCount;
                                                   FE.Var_Type  := To_Unbounded_String (VSym);
                                                   FE.Var_Field := VF_Idx;
                                                   Ctx.Fields.Include (Tree_Key (VCID), FE);
                                                   VF_Idx := VF_Idx + 1;
                                                end;
                                             end loop;
                                          end;
                                       end if;
                                    end loop;
                                 end if;
                              end;
                           end if;
                           if VF_Idx = 0 then
                              VF_Str := To_Unbounded_String ("i8");
                              V_Off := 1;
                              V_Al  := 1;
                           else
                              V_Off := Align_Up (V_Off, V_Al);
                           end if;
                           Add_Type_Decl (Ctx.E, "type " & VSym & " = { " & To_String (VF_Str) & " }");
                           if VCount > 0 then
                              Append (U_Fields, ", ");
                           end if;
                           Append (U_Fields, VSym);
                           if V_Off > U_Max_Sz then U_Max_Sz := V_Off; end if;
                           if V_Al > U_Max_Al then U_Max_Al := V_Al; end if;
                           VCount := VCount + 1;
                        end;
                     end if;
                  end loop;
                  if VCount > 0 then
                     U_Max_Sz := Align_Up (U_Max_Sz, U_Max_Al);
                     Add_Type_Decl (Ctx.E, "type " & Union_Sym & " = union { " & To_String (U_Fields) & " }");
                     if FIdx > 0 then
                        Append (Fields_S, ", ");
                     end if;
                     Append (Fields_S, Union_Sym);
                     Offset := Align_Up (Offset, U_Max_Al) + U_Max_Sz;
                     if U_Max_Al > Max_Al then Max_Al := U_Max_Al; end if;
                     FIdx := FIdx + 1;
                  end if;
               end;
            end if;
         end;
      end Walk_Comp_List_Fixed;
   begin
      if Ctx.Types.Contains (Key) then
         return Ctx.Types.Element (Key);
      end if;
      -- Placeholder to guard against recursive access-to-record definitions
      Info.Kind := TK_Record;
      Info.Pir_Type := To_Unbounded_String (TName);
      Info.Byte_Size := 8;
      Info.Align := 8;
      Ctx.Types.Include (Key, Info);

      -- Discriminants first
      declare
         DS : constant TREE := D (SM_DISCRIMINANT_S, Base_Rec);
      begin
         if not Is_Void_Tree (DS) then
            declare
               DL : SEQ_TYPE := LIST (DS);
               DD : TREE;
            begin
               while not IS_EMPTY (DL) loop
                  POP (DL, DD);
                  if KIND (DD) = DN_DSCRMT_DECL then
                     declare
                        Names : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                        DID   : TREE;
                     begin
                        while not IS_EMPTY (Names) loop
                           POP (Names, DID);
                           declare
                              FT : constant Type_Info := Get_Type_Info (Ctx, D (SM_OBJ_TYPE, DID));
                           begin
                              Append_Field (DID, FT);
                           end;
                        end loop;
                     end;
                  end if;
               end loop;
            end;
         end if;
      end;

      Walk_Comp_List_Fixed (D (SM_COMP_LIST, Base_Rec));

      if FIdx = 0 then
         -- Empty record (e.g. null;) -> single i8 dummy byte
         Fields_S := To_Unbounded_String ("i8");
         Offset := 1;
         Max_Al := 1;
      else
         Offset := Align_Up (Offset, Max_Al);
      end if;

      Add_Type_Decl (Ctx.E, "type " & TName & " = { " & To_String (Fields_S) & " }");
      Info.Byte_Size := Offset;
      Info.Align := Max_Al;
      Ctx.Types.Include (Key, Info);
      return Info;
   end Build_Record_Type;

   function Get_Type_Info (Ctx : in out Context; Raw_Type : TREE) return Type_Info is
      Sub : constant TREE := Subst_Tree (Ctx, Raw_Type);
      R   : constant TREE := Resolve_Type_Spec (Sub);
      Key : constant Unsigned_32 := Tree_Key (R);
      Info : Type_Info;
   begin
      if Is_Void_Tree (R) then
         Info.Kind := TK_Void;
         Info.Pir_Type := To_Unbounded_String ("void");
         return Info;
      end if;

      if Ctx.Types.Contains (Key) then
         return Ctx.Types.Element (Key);
      end if;

      if Is_Scalar_Type (R) then
         Info.Kind := TK_Scalar;
         Info.Pir_Type := To_Unbounded_String (Pir_Scalar_Type (R));
         declare
            Bits : constant Natural := Scalar_Bits (R);
         begin
            Info.Byte_Size := Bits / 8;
            if Info.Byte_Size = 0 then Info.Byte_Size := 1; end if;
            Info.Align := Info.Byte_Size;
         end;
         Ctx.Types.Include (Key, Info);
         return Info;

      elsif Is_Record_Type (R) then
         Info := Build_Record_Type (Ctx, R);
         Ctx.Types.Include (Key, Info);
         return Info;

      elsif Is_Constrained_Array (R) then
         declare
            Base   : constant TREE := Base_Type_Spec (R);
            Elem_T : constant TREE := Subst_Tree (Ctx, D (SM_COMP_TYPE, Base));
            EI     : constant Type_Info := Get_Type_Info (Ctx, Elem_T);
            Sub_S  : constant TREE := D (SM_INDEX_SUBTYPE_S, R);
            List_S : SEQ_TYPE := LIST (Sub_S);
            Idx_T  : TREE;
            Dim    : Natural := 0;
            Total  : Integer_64 := 1;
            All_St : Boolean := True;
         begin
            Info.Kind := TK_Constrained_Array;
            Info.Elem_Type := Elem_T;
            while not IS_EMPTY (List_S) and then Dim < 4 loop
               POP (List_S, Idx_T);
               Dim := Dim + 1;
               declare
                  St : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Get_Discrete_Bounds (Idx_T, St, Lo, Hi);
                  Info.Dims (Dim) := (Is_Static => St, First => Lo, Last => Hi, Idx_Type => Idx_T);
                  if St then
                     if Hi >= Lo then
                        Total := Total * (Hi - Lo + 1);
                     else
                        Total := 0;
                     end if;
                  else
                     All_St := False;
                  end if;
               end;
            end loop;
            Info.NDims := Dim;
            Info.Align := EI.Align;
            if All_St then
               Info.Is_Dynamic := False;
               declare
                  N_Elems : constant Long_Long_Integer :=
                    Long_Long_Integer'Max (1, Long_Long_Integer (Total));
                  TName   : constant String := "@ada_arr_" & Tree_Key_Str (R);
               begin
                  Add_Type_Decl (Ctx.E,
                    "type " & TName & " = [" & Img (N_Elems) & " x " & To_String (EI.Pir_Type) & "]");
                  Info.Pir_Type := To_Unbounded_String (TName);
                  Info.Byte_Size := Natural (N_Elems) * Align_Up (EI.Byte_Size, EI.Align);
               end;
            else
               Info.Is_Dynamic := True;
               Info.Pir_Type := To_Unbounded_String ("ptr");
               Info.Byte_Size := 8;
               Info.Align := 8;
            end if;
            Ctx.Types.Include (Key, Info);
            return Info;
         end;

      elsif Is_Unconstrained_Array (R) then
         declare
            Elem_T : constant TREE := Subst_Tree (Ctx, D (SM_COMP_TYPE, R));
            Idx_S  : constant TREE := D (SM_INDEX_S, R);
            IL     : SEQ_TYPE := LIST (Idx_S);
            Idx_N  : TREE;
            Dim    : Natural := 0;
         begin
            Info.Kind := TK_Unconstrained_Array;
            Info.Elem_Type := Elem_T;
            while not IS_EMPTY (IL) and then Dim < 4 loop
               POP (IL, Idx_N);
               Dim := Dim + 1;
               Info.Dims (Dim).Idx_Type := D (SM_TYPE_SPEC, Idx_N);
            end loop;
            Info.NDims := Dim;
            Info.Pir_Type := To_Unbounded_String ("ptr");
            Info.Byte_Size := 8;
            Info.Align := 8;
            Ctx.Types.Include (Key, Info);
            return Info;
         end;

      else
         Info.Kind := TK_Scalar;
         Info.Pir_Type := To_Unbounded_String ("i64");
         Info.Byte_Size := 8;
         Info.Align := 8;
         Ctx.Types.Include (Key, Info);
         return Info;
      end if;
   end Get_Type_Info;

   function Mangle_Subp_Name (Ctx : in out Context; Subp_Id : TREE; Spec_Node : TREE) return String is
      Can  : constant TREE := Canonical_Def (Subp_Id);
      Base : constant String := Qualified_Ada_Name (Can);
      Sig  : Unbounded_String := To_Unbounded_String ("@ada__" & Base);
   begin
      if not Is_Void_Tree (Spec_Node) then
         declare
            PS : constant TREE := D (AS_PARAM_S, Spec_Node);
            PL : SEQ_TYPE;
            P  : TREE;
         begin
            if not Is_Void_Tree (PS) then
               PL := LIST (PS);
               while not IS_EMPTY (PL) loop
                  POP (PL, P);
                  declare
                     NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                     PID : TREE;
                  begin
                     while not IS_EMPTY (NS) loop
                        POP (NS, PID);
                        declare
                           PT : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                           TI : constant Type_Info := Get_Type_Info (Ctx, PT);
                        begin
                           Append (Sig, "__");
                           if TI.Kind = TK_Scalar then
                              Append (Sig, To_String (TI.Pir_Type));
                           elsif TI.Kind = TK_Record then
                              Append (Sig, "rec");
                           elsif TI.Kind = TK_Unconstrained_Array then
                              Append (Sig, "uarr" & Img (TI.NDims));
                           else
                              Append (Sig, "arr");
                           end if;
                        end;
                     end loop;
                  end;
               end loop;
            end if;
         end;
         if KIND (Spec_Node) = DN_FUNCTION_SPEC then
            declare
               RT : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, Spec_Node))));
               TI : constant Type_Info := Get_Type_Info (Ctx, RT);
            begin
               Append (Sig, "__ret_");
               if TI.Kind = TK_Scalar then
                  Append (Sig, To_String (TI.Pir_Type));
               else
                  Append (Sig, "comp");
               end if;
            end;
         end if;
      end if;
      -- Disambiguate local/nested subprograms by key only if nested inside another subprogram
      declare
         Reg : constant TREE := D (XD_REGION, Can);
      begin
         if not Is_Void_Tree (Reg) and then KIND (Reg) /= DN_PACKAGE_ID then
            Append (Sig, "_" & Tree_Key_Str (Can));
         end if;
      end;
      return To_String (Sig);
   end Mangle_Subp_Name;

   function Has_Active_Handler (Ctx : Context) return Boolean is
   begin
      if Ctx.Handlers.Is_Empty then
         return False;
      end if;
      for I in reverse 0 .. Natural (Ctx.Handlers.Length) - 1 loop
         if not Ctx.Handlers.Element (I).In_Handler then
            return True;
         end if;
      end loop;
      return False;
   end Has_Active_Handler;

   function Active_Handler_Label (Ctx : Context) return String is
   begin
      for I in reverse 0 .. Natural (Ctx.Handlers.Length) - 1 loop
         if not Ctx.Handlers.Element (I).In_Handler then
            return To_String (Ctx.Handlers.Element (I).Handler_Label);
         end if;
      end loop;
      return "";
   end Active_Handler_Label;

   procedure Emit_Raise_Reg (Ctx : in out Context; Exn_Reg : String) is
   begin
      if Has_Active_Handler (Ctx) then
         Emit_Term (Ctx.E, "jmp " & Active_Handler_Label (Ctx) & "(" & Exn_Reg & ")");
      else
         Emit_Term (Ctx.E, "resume " & Exn_Reg);
      end if;
   end Emit_Raise_Reg;

   procedure Emit_Raise_Tag (Ctx : in out Context; Tag_Sym : String) is
      R : constant String := Next_Reg (Ctx.E, "exc");
   begin
      Emit_Inst (Ctx.E, R & " = addr " & Tag_Sym);
      Emit_Raise_Reg (Ctx, R);
   end Emit_Raise_Tag;

   procedure Emit_Call_Or_Invoke
     (Ctx       : in out Context;
      Func_Sym  : String;
      Args_Str  : String;
      Ret_Type  : String;
      Res_Reg   : out Unbounded_String;
      Nounwind  : Boolean := False)
   is
   begin
      Res_Reg := Null_Unbounded_String;
      if Has_Active_Handler (Ctx) and then not Nounwind then
         declare
            Ok_Lbl  : constant String := Next_Label (Ctx.E, "ok");
            Hdl_Lbl : constant String := Active_Handler_Label (Ctx);
         begin
            if Ret_Type'Length = 0 or else Ret_Type = "void" then
               Emit_Term (Ctx.E,
                 "invoke " & Func_Sym & "(" & Args_Str & ") then " & Ok_Lbl &
                 " unwind " & Hdl_Lbl & "(exn)");
               Emit_Label (Ctx.E, Ok_Lbl);
            else
               declare
                  R : constant String := Next_Reg (Ctx.E, "rv");
               begin
                  Emit_Term (Ctx.E,
                    "invoke " & Func_Sym & "(" & Args_Str & ") then " & Ok_Lbl &
                    "(ret) unwind " & Hdl_Lbl & "(exn)");
                  Emit_Label (Ctx.E, Ok_Lbl, R & ": " & Ret_Type);
                  Res_Reg := To_Unbounded_String (R);
               end;
            end if;
         end;
      else
         if Ret_Type'Length = 0 or else Ret_Type = "void" then
            Emit_Inst (Ctx.E, "call " & Func_Sym & "(" & Args_Str & ")");
         else
            declare
               R : constant String := Next_Reg (Ctx.E, "rv");
            begin
               Emit_Inst (Ctx.E, R & " = call " & Func_Sym & "(" & Args_Str & ")");
               Res_Reg := To_Unbounded_String (R);
            end;
         end if;
      end if;
   end Emit_Call_Or_Invoke;

   function Build_Param_Sig
     (Ctx        : in out Context;
      Spec_Node  : TREE;
      Needs_SL   : Boolean;
      Ret_Comp   : Boolean;
      Ret_Type   : TREE;
      With_Names : Boolean) return String
   is
      Buf   : Unbounded_String;
      First : Boolean := True;

      procedure Add_P (Name : String; Ty : String) is
      begin
         if not First then
            Append (Buf, ", ");
         end if;
         First := False;
         if With_Names then
            Append (Buf, Name & ": " & Ty);
         else
            Append (Buf, Name & ": " & Ty);
         end if;
      end Add_P;
   begin
      if Needs_SL then
         Add_P ("%__sl", "ptr");
      end if;
      if Ret_Comp then
         Add_P ("%__ret", "ptr");
      end if;
      if not Is_Void_Tree (Spec_Node) then
         declare
            PS  : constant TREE := D (AS_PARAM_S, Spec_Node);
            PL  : SEQ_TYPE;
            P   : TREE;
            Idx : Natural := 0;
         begin
            if not Is_Void_Tree (PS) then
               PL := LIST (PS);
               while not IS_EMPTY (PL) loop
                  POP (PL, P);
                  declare
                     Mode : constant NODE_NAME := KIND (P);
                     NS   : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                     PID  : TREE;
                  begin
                     while not IS_EMPTY (NS) loop
                        POP (NS, PID);
                        declare
                           PT : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                           TI : constant Type_Info := Get_Type_Info (Ctx, PT);
                           PN : constant String := "%p_" & Sanitize_Ident (Sym_Name (PID)) & "_" & Img (Idx);
                        begin
                           if TI.Kind = TK_Unconstrained_Array then
                              Add_P (PN & "_ptr", "ptr");
                              for D_Idx in 1 .. TI.NDims loop
                                 Add_P (PN & "_f" & Img (D_Idx), "i32");
                                 Add_P (PN & "_l" & Img (D_Idx), "i32");
                              end loop;
                           elsif TI.Kind = TK_Record or else TI.Kind = TK_Constrained_Array then
                              Add_P (PN, "ptr");
                           elsif Mode = DN_OUT or else Mode = DN_IN_OUT then
                              Add_P (PN, "ptr");
                           else
                              Add_P (PN, To_String (TI.Pir_Type));
                           end if;
                           Idx := Idx + 1;
                        end;
                     end loop;
                  end;
               end loop;
            end if;
         end;
      end if;
      pragma Unreferenced (Ret_Type);
      return To_String (Buf);
   end Build_Param_Sig;

   procedure Ensure_Extern_Subp (Ctx : in out Context; Subp_Id : TREE; Info : Subp_Info) is
      Sym : constant String := To_String (Info.Pir_Name);
      Ret : Unbounded_String;
      Params : constant String :=
        Build_Param_Sig (Ctx, Info.Spec_Node, Info.Needs_SL, Info.Ret_Composite, Info.Ret_Type, True);
   begin
      if Has_Symbol (Ctx.E, Sym) then
         return;
      end if;
      if not Info.Is_Function or else Info.Ret_Composite then
         Ret := To_Unbounded_String ("void");
      else
         Ret := Get_Type_Info (Ctx, Info.Ret_Type).Pir_Type;
      end if;
      Add_Extern_Func (Ctx.E, Sym,
        "extern func " & Sym & "(" & Params & ") -> " & To_String (Ret) & " cc ""internal""");
      pragma Unreferenced (Subp_Id);
   end Ensure_Extern_Subp;

   function Emit_Var_Addr (Ctx : in out Context; V : Var_Info) return String is
   begin
      if V.Kind = VK_Global then
         declare
            R : constant String := Next_Reg (Ctx.E, "ga");
         begin
            Emit_Inst (Ctx.E, R & " = addr " & To_String (V.Pir_Name));
            return R;
         end;
      elsif V.Owner_Subp = Ctx.Cur_Subp_Key or else V.Owner_Subp = 0 then
         return To_String (V.Pir_Name);
      else
         -- Hoist & cache uplevel variable address load in .entry (invariant across entire subprogram!)
         declare
            Cache_Key : constant Unsigned_32 :=
              Unsigned_32 (V.Owner_Subp * 31) xor Unsigned_32 (V.Captured_Slot + 1);
         begin
            if Ctx.Uplevel_Cache.Contains (Cache_Key) then
               return To_String (Ctx.Uplevel_Cache.Element (Cache_Key).Pir_Type);
            end if;
            declare
               Cur_K : Unsigned_32 := Ctx.Cur_Subp_Key;
               SL    : Unbounded_String := Ctx.Cur_SL_Reg;
            begin
               for Hop in 1 .. 16 loop
                  declare
                     SI : constant Subp_Info := Ctx.Subps.Element (Cur_K);
                     Parent_K : constant Unsigned_32 := SI.Parent_Subp;
                  begin
                     if Parent_K = V.Owner_Subp then
                        declare
                           PI : constant Subp_Info := Ctx.Subps.Element (Parent_K);
                           FP : constant String := Next_Reg (Ctx.E, "ufp");
                           VA : constant String := Next_Reg (Ctx.E, "uva");
                           CE : Type_Info;
                        begin
                           Emit_Entry_Alloca (Ctx.E,
                             FP & " = field_ptr " & To_String (PI.Frame_Type) & ", " &
                             To_String (SL) & ", " & Img (V.Captured_Slot));
                           Emit_Entry_Alloca (Ctx.E, VA & " = load.ptr " & FP & ", align 8");
                           CE.Pir_Type := To_Unbounded_String (VA);
                           Ctx.Uplevel_Cache.Include (Cache_Key, CE);
                           return VA;
                        end;
                     else
                        declare
                           PI  : constant Subp_Info := Ctx.Subps.Element (Parent_K);
                           FP  : constant String := Next_Reg (Ctx.E, "slp");
                           NSL : constant String := Next_Reg (Ctx.E, "sl");
                        begin
                           Emit_Entry_Alloca (Ctx.E,
                             FP & " = field_ptr " & To_String (PI.Frame_Type) & ", " &
                             To_String (SL) & ", 0");
                           Emit_Entry_Alloca (Ctx.E, NSL & " = load.ptr " & FP & ", align 8");
                           SL := To_Unbounded_String (NSL);
                           Cur_K := Parent_K;
                        end;
                     end if;
                  end;
               end loop;
            end;
         end;
         return To_String (V.Pir_Name);
      end if;
   end Emit_Var_Addr;

end Cg_State;
