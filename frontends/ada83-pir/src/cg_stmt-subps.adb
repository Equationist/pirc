-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with Ada.Directories;
with Ada.Strings.Fixed;
with Ada.Strings.Maps.Constants;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with Ada_Sym_Util;
with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr; use Cg_Expr;
with Lib_Binder;

package body Cg_Stmt.Subps is

   -- =========================================================================
   -- Subprogram Body Emission (with Static-Link Frame Setup)
   -- =========================================================================

   procedure Emit_Subprogram_Body
     (Ctx           : in out Context;
      Body_Node     : TREE;
      Override_Id   : TREE := CONST_VOID;
      Override_Spec : TREE := CONST_VOID;
      Is_Exported   : Boolean := False;
      Is_Subunit    : Boolean := False)
   is
      Subp_Id   : constant TREE :=
        (if not Is_Void_Tree (Override_Id) then Canonical_Def (Override_Id)
         else Canonical_Def (Subst_Tree (Ctx, Canonical_Def (D (AS_SOURCE_NAME, Body_Node)))));
      Spec_Node : constant TREE :=
        (if not Is_Void_Tree (Override_Spec) then Override_Spec
         elsif Subp_Id /= Canonical_Def (D (AS_SOURCE_NAME, Body_Node))
           and then not Is_Void_Tree (D (SM_SPEC, Subp_Id))
         then D (SM_SPEC, Subp_Id)
         else D (AS_HEADER, Body_Node));
      Subp_Key  : constant Unsigned_32 := Tree_Key (Subp_Id);
      Block_B   : constant TREE := D (AS_BODY, Body_Node);
      SI        : Subp_Info;

      -- Save outer subprogram context (to support emitting nested subprograms cleanly)
      Saved_Buf      : Pir_Emit.Func_Buffer;
      Saved_State    : constant Subp_State_Snapshot := Save_Subp_State (Ctx);
      Saved_Key      : constant Unsigned_32 := Saved_State.Cur_Subp_Key;

      Captured_Vars  : Cap_Vecs.Vector;
      Nested_Subps   : U32_Vecs.Vector;
   begin
      declare
         Bdy_Hdr : constant TREE := D (AS_HEADER, Body_Node);
      begin
         if not Is_Void_Tree (Bdy_Hdr) and then not Is_Void_Tree (Spec_Node) and then Bdy_Hdr /= Spec_Node then
            declare
               BPS : constant TREE := D (AS_PARAM_S, Bdy_Hdr);
               SPS : constant TREE := D (AS_PARAM_S, Spec_Node);
            begin
               if not Is_Void_Tree (BPS) and then not Is_Void_Tree (SPS) then
                  declare
                     BL : SEQ_TYPE := LIST (BPS);
                     SL : SEQ_TYPE := LIST (SPS);
                     BP, SP : TREE;
                  begin
                     while not IS_EMPTY (BL) and then not IS_EMPTY (SL) loop
                        POP (BL, BP); POP (SL, SP);
                        declare
                           BNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, BP));
                           SNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, SP));
                           BID, SID_N : TREE;
                        begin
                           while not IS_EMPTY (BNS) and then not IS_EMPTY (SNS) loop
                              POP (BNS, BID); POP (SNS, SID_N);
                              Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (BID)), Canonical_Def (SID_N));
                              Ctx.Generic_Subst.Include (Tree_Key (BID), Canonical_Def (SID_N));
                           end loop;
                        end;
                     end loop;
                  end;
               end if;
            end;
         end if;
      end;

      -- P3-5.3: Inline separate subunit belonging to a generic template BEFORE Eff_Subunit / Needs_SL:=False!
      if not Is_Void_Tree (Block_B) and then KIND (Block_B) = DN_STUB then
         declare
            Sub_Body : constant TREE := Find_Gen_Subunit_Body (Ctx, Body_Node);
         begin
            if not Is_Void_Tree (Sub_Body) and then KIND (Sub_Body) = DN_SUBPROGRAM_BODY
              and then not Is_Void_Tree (D (AS_BODY, Sub_Body))
              and then KIND (D (AS_BODY, Sub_Body)) /= DN_STUB
            then
               Emit_Subprogram_Body (Ctx, Sub_Body,
                 Override_Id   => Subp_Id,
                 Override_Spec => Spec_Node,
                 Is_Exported   => Is_Exported,
                 Is_Subunit    => Is_Subunit);
               return;
            end if;
         end;
      end if;

      declare
         Was_Pre_Reg : constant Boolean := Ctx.Subps.Contains (Subp_Key);
         Eff_Subunit : constant Boolean := Is_Subunit or else Ctx.Has_Stubs or else (not Is_Void_Tree (Block_B) and then KIND (Block_B) = DN_STUB);
      begin
         Pre_Register_Subp (Ctx, Subp_Id, Spec_Node, Saved_Key, Is_Subunit => Eff_Subunit);
         SI := Ctx.Subps.Element (Subp_Key);
         if not Is_Void_Tree (Block_B) and then KIND (Block_B) = DN_STUB then
            SI.Needs_SL := False;
            Ctx.Subps.Replace (Subp_Key, SI);
         end if;
         if not Was_Pre_Reg and then not Eff_Subunit and then Length (Ctx.Gen_Inst_Tag) > 0 and then not Is_Library_Level (Subp_Id) then
            SI.Pir_Name := To_Unbounded_String (Mangle_Subp_Name (Ctx, Subp_Id, Spec_Node));
            Ctx.Subps.Replace (Subp_Key, SI);
         end if;
      end;

      -- Handle external separate subunit stub: declare extern and skip
      if Is_Void_Tree (Block_B) or else KIND (Block_B) = DN_STUB then
         Ensure_Extern_Subp (Ctx, Subp_Id, SI);
         return;
      end if;

      -- Pre-register local variables, block variables, loop variables, & parameters for static-link capture scan
      declare
         Temp_Owners : Var_Maps.Map;
         Local_Meta  : U32_Vecs.Vector;
         PS : constant TREE := D (AS_PARAM_S, Spec_Node);

         procedure Collect_Local_Owners (Node : TREE; In_Gen_Bdy : Boolean := False) is
            K : NODE_NAME;
         begin
            if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then
               return;
            end if;
            K := KIND (Node);
            if K = DN_SUBPROGRAM_BODY or else K = DN_PACKAGE_BODY then
               declare
                  SN : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
               begin
                  if not Is_Void_Tree (SN) and then KIND (SN) = DN_GENERIC_ID then
                     if not Ctx.Generic_Body.Contains (Tree_Key (SN))
                       or else Is_Void_Tree (D (AS_BODY, Node))
                       or else KIND (D (AS_BODY, Node)) /= DN_STUB
                     then
                        Ctx.Generic_Body.Include (Tree_Key (SN), Node);
                     end if;
                  end if;
               end;
                  if K = DN_SUBPROGRAM_BODY then
                     return;
                  end if;
                  if K = DN_PACKAGE_BODY then
                     declare
                        Sub_P : constant TREE := Find_Gen_Subunit_Body (Ctx, Node);
                     begin
                        if not Is_Void_Tree (Sub_P) then
                           Collect_Local_Owners (D (AS_BODY, Sub_P), In_Gen_Bdy);
                           return;
                        end if;
                     end;
                  end if;
            end if;
            if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then
               return;
            end if;
            if K = DN_TYPE_DECL then
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS  : constant TREE :=
                    (if not Is_Void_Tree (TID) and then KIND (TID) in CLASS_TYPE_NAME
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TID))) else CONST_VOID);
               begin
                  if not Is_Void_Tree (TS) and then KIND (TS) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                    and then Type_Contains_Task (Ctx, D (SM_DESIG_TYPE, TS))
                  then
                     declare
                        BS : constant TREE := Base_Type_Spec (TS);
                     begin
                        if not Is_Void_Tree (BS) and then KIND (BS) = DN_ACCESS and then Is_Void_Tree (D (SM_DERIVED, BS)) then
                           Local_Meta.Append (Tree_Key (BS));
                        end if;
                     end;
                  end if;
               end;
               return;
            elsif K = DN_VARIABLE_DECL or else K = DN_CONSTANT_DECL then
               declare
                  NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Node));
                  ID : TREE;
               begin
                  while not IS_EMPTY (NS) loop
                     POP (NS, ID);
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := Subp_Key;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                     end;
                  end loop;
               end;
               return;
            elsif K = DN_TASK_DECL then
               declare
                  Src : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS_M : TREE := CONST_VOID;
               begin
                  if not Is_Void_Tree (Src) and then KIND (Src) = DN_VARIABLE_ID then
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := Subp_Key;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (Src)), V);
                     end;
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src)));
                  elsif not Is_Void_Tree (Src) and then KIND (Src) in CLASS_TYPE_NAME then
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Src)));
                  end if;
                  if not Is_Void_Tree (TS_M) and then KIND (TS_M) = DN_TASK_SPEC then
                     Local_Meta.Append (Tree_Key (TS_M));
                  end if;
               end;
               return;
            elsif K = DN_LENGTH_ENUM_REP then
               -- P3-6.6: Capture local dynamic 'STORAGE_SIZE slots in parent static-link frame
               declare
                  AN : constant TREE := D (AS_NAME, Node);
                  EN : constant TREE := D (AS_EXP, Node);
               begin
                  if not Is_Void_Tree (AN) and then KIND (AN) = DN_ATTRIBUTE and then not Is_Void_Tree (EN)
                    and then not Is_Void_Tree (D (AS_USED_NAME_ID, AN))
                    and then Sym_Name (D (AS_USED_NAME_ID, AN)) = "STORAGE_SIZE"
                  then
                     Local_Meta.Append (Tree_Key (EN));
                  end if;
               end;
               return;
            elsif K = DN_RENAMES_OBJ_DECL then
               declare
                  ID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := Subp_Key;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
               return;
            elsif K = DN_FOR or else K = DN_REVERSE then
               declare
                  ID : constant TREE := D (AS_SOURCE_NAME, Node);
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := Subp_Key;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
               return;
            elsif K = DN_SUBPROG_ENTRY_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     Collect_Local_Owners (D (SM_DECL_S, UK), In_Gen_Bdy);
                     return;
                  elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                     declare
                        SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        V   : Var_Info;
                     begin
                        V.Owner_Subp    := Subp_Key;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (SID), V);
                     end;
                     return;
                  end if;
               end;
            elsif K = DN_PACKAGE_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     declare
                        PID    : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                        GK     : constant Unsigned_32 := Tree_Key (Gen_Id);
                        GB     : TREE := CONST_VOID;
                     begin
                        Collect_Local_Owners (D (SM_DECL_S, UK), In_Gen_Bdy);
                        Collect_Local_Owners (D (SM_SPEC, PID), In_Gen_Bdy);
                        -- P3-5.1: Propagate nested generic bodies in instantiated package spec
                        declare
                           GSp : constant TREE := (if not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_SPEC, Gen_Id) else CONST_VOID);
                           ISp : constant TREE := D (SM_SPEC, PID);
                           Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
                        begin
                           if not Is_Void_Tree (GSp) and then KIND (GSp) = DN_PACKAGE_SPEC
                             and then not Is_Void_Tree (ISp) and then KIND (ISp) = DN_PACKAGE_SPEC
                           then
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, GSp), D (AS_DECL_S1, ISp));
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, GSp), D (AS_DECL_S2, ISp));
                           end if;
                           Ctx.Generic_Subst := Saved_Sub;
                        end;
                        if Ctx.Generic_Body.Contains (GK) then
                           GB := Ctx.Generic_Body.Element (GK);
                        elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then
                           GB := D (SM_BODY, Gen_Id);
                           if Is_Void_Tree (GB) then
                              GB := D (XD_BODY, Gen_Id);
                           end if;
                        end if;
                        if not Is_Void_Tree (GB) then
                           if KIND (GB) = DN_PACKAGE_BODY then
                              Collect_Local_Owners (D (AS_BODY, GB), True);
                           else
                              Collect_Local_Owners (GB, True);
                           end if;
                        end if;
                        return;
                     end;
                  end if;
               end;
            end if;
            if ARITY (Node) = ARBITRARY then
               declare
                  L    : SEQ_TYPE := LIST (Node);
                  Item : TREE;
               begin
                  while not IS_EMPTY (L) loop
                     POP (L, Item);
                     Collect_Local_Owners (Item, In_Gen_Bdy);
                  end loop;
               end;
            else
               for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
                  Collect_Local_Owners (DABS (I, Node), In_Gen_Bdy);
               end loop;
            end if;
         end Collect_Local_Owners;
      begin
         if not Is_Void_Tree (PS) then
            declare
               PL : SEQ_TYPE := LIST (PS);
               P  : TREE;
            begin
               while not IS_EMPTY (PL) loop
                  POP (PL, P);
                  declare
                     NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                     ID : TREE;
                  begin
                     while not IS_EMPTY (NS) loop
                        POP (NS, ID);
                        declare
                           V : Var_Info;
                        begin
                           V.Owner_Subp := Subp_Key;
                           Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                        end;
                     end loop;
                  end;
               end loop;
            end;
         end if;
         Collect_Local_Owners (Block_B);
         Scan_Tree_For_Captures (Ctx, Block_B, Subp_Key, Temp_Owners, Ctx.Subps, Captured_Vars, Nested_Subps, Subp_Key);
         -- P3-4.3: Capture local access-master & task-elab slots in frame whenever nested subprograms/tasks exist
         if not Local_Meta.Is_Empty and then (not Nested_Subps.Is_Empty or else Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Block_B))) then
            for MK of Local_Meta loop
               declare
                  Found_M : Boolean := False;
               begin
                  for CP of Captured_Vars loop
                     if CP.Inst_Key = 0 and then CP.Var_Key = MK then
                        Found_M := True;
                        exit;
                     end if;
                  end loop;
                  if not Found_M then
                     Captured_Vars.Append ((Inst_Key => 0, Var_Key => MK));
                  end if;
               end;
            end loop;
         end if;
      end;

      -- Reload SI (Needs_SL might have been set by outer scan!)
      SI := Ctx.Subps.Element (Subp_Key);
      if not Captured_Vars.Is_Empty or else SI.Needs_SL then
         -- Propagate Needs_SL to all nested subprograms inside this subprogram so sibling/grandchild calls always have a valid SL!
         for NK of Nested_Subps loop
            if Ctx.Subps.Contains (NK) then
               declare
                  NSI : Subp_Info := Ctx.Subps.Element (NK);
               begin
                  NSI.Needs_SL := True;
                  Ctx.Subps.Replace (NK, NSI);
               end;
            end if;
         end loop;
      end if;
      if not Captured_Vars.Is_Empty or else (SI.Needs_SL and then not Nested_Subps.Is_Empty) then
         SI.Has_Frame := True;
         SI.Frame_Type := To_Unbounded_String ("@__frame_" & Tree_Key_Str (Subp_Id) & To_String (Ctx.Gen_Inst_Tag));
         -- Emit frame struct: { ptr parent_sl, ptr cap1..capN, ptr con1..conN }
         declare
            N_Cap     : constant Natural := Natural (Captured_Vars.Length);
            FT_Decl   : Unbounded_String := To_Unbounded_String ("type " & To_String (SI.Frame_Type) & " = { ptr");
         begin
            for Slot in 1 .. N_Cap loop
               Append (FT_Decl, ", ptr");
            end loop;
            for Slot in 1 .. N_Cap loop
               Append (FT_Decl, ", ptr");
               declare
                  CP : constant Cap_Pair := Captured_Vars.Element (Slot - 1);
                  CK : constant Unsigned_32 := CP.Var_Key;
               begin
                  if CP.Inst_Key /= 0 then
                     Ctx.Gen_Cap_Slots.Include
                       (Shift_Left (Unsigned_64 (CP.Inst_Key), 32) or Unsigned_64 (CK),
                        (Cap_Slot => Slot, Con_Cap_Slot => N_Cap + Slot));
                  else
                     declare
                        V : Var_Info;
                     begin
                        if Ctx.Vars.Contains (CK) then
                           V := Ctx.Vars.Element (CK);
                        end if;
                        V.Captured_Slot     := Slot;
                        V.Con_Captured_Slot := N_Cap + Slot;
                        V.Owner_Subp        := Subp_Key;
                        Ctx.Vars.Include (CK, V);
                     end;
                  end if;
               end;
            end loop;
            Append (FT_Decl, " }");
            Add_Type_Decl (Ctx.E, To_String (FT_Decl));
         end;
         Ctx.Subps.Replace (Subp_Key, SI);
      end if;

      -- Build function header
      declare
         Link   : constant String := (if Is_Exported then "export " else "");
         Params : constant String :=
           Build_Param_Sig (Ctx, Spec_Node, SI.Needs_SL, SI.Ret_Composite, SI.Ret_Type, True);
         Ret_S  : constant String :=
           (if not SI.Is_Function or else SI.Ret_Composite then "void"
            else To_String (Get_Type_Info (Ctx, SI.Ret_Type).Pir_Type));
         Hdr    : constant String :=
           Link & "func " & To_String (SI.Pir_Name) & "(" & Params & ") -> " & Ret_S & " cc ""internal""";
      begin
         Begin_Func (Ctx.E, Saved_Buf, Hdr);
      end;

      Ctx.Cur_Subp_Key  := Subp_Key;
      Ctx.Cur_Subp_Node := Body_Node;
      Ctx.Cur_Is_Func   := SI.Is_Function;
      Ctx.Cur_Ret_Type  := SI.Ret_Type;
      Ctx.Cur_Ret_Comp  := SI.Ret_Composite;
      Ctx.Cur_Ret_Uncon := SI.Ret_Uncon_Arr;
      Ctx.Cur_Ret_Ptr   := To_Unbounded_String (if SI.Ret_Composite then "%__ret" else "");
      Ctx.Cur_SL_Reg    := To_Unbounded_String (if SI.Needs_SL then "%__sl" else "");
      Reset_Subp_State (Ctx);
      if not Is_Void_Tree (D (AS_BODY, Body_Node)) and then KIND (D (AS_BODY, Body_Node)) = DN_BLOCK_BODY then
         declare
            BB    : constant TREE := D (AS_BODY, Body_Node);
            InitD : constant Natural :=
              (if Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, BB)) then 1 else 0) +
              (if not SI.Ret_Uncon_Arr and then Tree_Uses_SS (Ctx, BB) then 1 else 0);
         begin
            Scan_Label_Cleanup_Depths (Ctx, D (AS_STM_S, BB), InitD);
            Scan_Label_Cleanup_Depths (Ctx, D (AS_ALTERNATIVE_S, BB), InitD);
            -- P3-4.14: Apply pragma PRIORITY for a main subprogram (LRM 9.8(2))
            if Is_Exported and then not Is_Subunit and then Is_Library_Level (Subp_Id)
              and then not Is_Void_Tree (D (AS_ITEM_S, BB))
            then
               declare
                  IL : SEQ_TYPE := LIST (D (AS_ITEM_S, BB));
                  It : TREE;
               begin
                  while not IS_EMPTY (IL) loop
                     POP (IL, It);
                     if KIND (It) = DN_PRAGMA then
                        declare
                           PID : constant TREE := D (AS_USED_NAME_ID, It);
                           PAS : constant TREE := D (AS_GENERAL_ASSOC_S, It);
                        begin
                           if not Is_Void_Tree (PID) and then Sym_Name (PID) = "PRIORITY"
                             and then not Is_Void_Tree (PAS) and then not IS_EMPTY (LIST (PAS))
                           then
                              declare
                                 A1 : TREE := HEAD (LIST (PAS));
                                 SV : TREE;
                                 Dummy_P : Unbounded_String;
                              begin
                                 if KIND (A1) = DN_ASSOC then A1 := D (AS_EXP, A1); end if;
                                 SV := Safe_SM_Value (A1);
                                 if Has_Static_Val (SV) and then KIND (SV) = DN_NUM_VAL then
                                    declare
                                       PV : constant Integer_64 := Eval_Int64 (SV);
                                    begin
                                       if PV >= 0 and then PV <= 10 then
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_set_main_priority", Img (Long_Long_Integer (PV)), "", Dummy_P, Nounwind => True);
                                       end if;
                                    end;
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                  end loop;
               end;
            end if;
         end;
      end if;

      -- LRM 3.9: check subprogram body has been elaborated before call (c39006*, ca5006a)
      if Ctx.Enable_Checks and then Length (SI.Elab_Flag) > 0 then
         declare
            FA    : constant String := Next_Reg (Ctx.E, "sfa");
            FV    : constant String := Next_Reg (Ctx.E, "sfv");
            NE    : constant String := Next_Reg (Ctx.E, "sne");
            L_Bad : constant String := Next_Label (Ctx.E, "spe_b");
            L_Ok  : constant String := Next_Label (Ctx.E, "spe_o");
         begin
            Emit_Inst (Ctx.E, FA & " = addr " & To_String (SI.Elab_Flag));
            Emit_Inst (Ctx.E, FV & " = load.i8 " & FA & ", align 1");
            Emit_Inst (Ctx.E, NE & " = cmp_eq.i8 " & FV & ", 0");
            Emit_Term (Ctx.E, "br " & NE & ", " & L_Bad & ", " & L_Ok);
            Emit_Label (Ctx.E, L_Bad);
            Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
            Emit_Label (Ctx.E, L_Ok);
         end;
      end if;

      if SI.Has_Frame then
         declare
            FR : constant String := Next_Reg (Ctx.E, "fr");
            FP : constant String := Next_Reg (Ctx.E, "frp");
         begin
            Emit_Entry_Alloca (Ctx.E, FR & " = alloca " & To_String (SI.Frame_Type) & ", align 8");
            Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " & FR & ", 0");
            if SI.Needs_SL then
               Emit_Inst (Ctx.E, "store.ptr %__sl, " & FP & ", align 8");
            else
               Emit_Inst (Ctx.E, "store.ptr 0, " & FP & ", align 8");
            end if;
            Ctx.Cur_Frame_Reg := To_Unbounded_String (FR);
         end;
      end if;

      -- Bind formal parameters into Ctx.Vars
      -- If this subprogram declares direct separate subunits (DN_STUB), export scalar
      -- formal parameters via Dijkstra-display globals (saved on entry, restored on exit)
      -- so child subunits can access both non-recursive and recursive frames!
      declare
         Subp_Has_Stubs : Boolean := False;
      begin
         if not Is_Void_Tree (Block_B) and then KIND (Block_B) /= DN_STUB then
            declare
               IS_N : constant TREE := D (AS_ITEM_S, Block_B);
            begin
               if not Is_Void_Tree (IS_N) then
                  declare
                     SL : SEQ_TYPE := LIST (IS_N);
                     IT : TREE;
                  begin
                     while not IS_EMPTY (SL) loop
                        POP (SL, IT);
                        if (KIND (IT) = DN_SUBPROGRAM_BODY or else KIND (IT) = DN_PACKAGE_BODY or else KIND (IT) = DN_TASK_BODY)
                          and then not Is_Void_Tree (D (AS_BODY, IT))
                          and then KIND (D (AS_BODY, IT)) = DN_STUB
                          and then not Is_Gen_Subunit_Stub (Ctx, IT)
                        then
                           Subp_Has_Stubs := True;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;
            end;
         end if;

      if not Is_Void_Tree (Spec_Node) then
         declare
            PS  : constant TREE := D (AS_PARAM_S, Spec_Node);
            PL  : SEQ_TYPE := (if Is_Void_Tree (PS) then MAKE else LIST (PS));
            P   : TREE;
            Idx : Natural := 0;
         begin
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
                        Can_P : constant TREE := Canonical_Def (PID);
                        PK    : constant Unsigned_32 := Tree_Key (Can_P);
                        PT    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                        TI    : constant Type_Info := Get_Type_Info (Ctx, PT);
                        PN    : constant String := "%p_" & Sanitize_Ident (Sym_Name (PID)) & "_" & Img (Idx);
                        VInfo : Var_Info;
                     begin
                        if Ctx.Vars.Contains (PK) and then Ctx.Vars.Element (PK).Owner_Subp = Subp_Key then
                           VInfo.Captured_Slot     := Ctx.Vars.Element (PK).Captured_Slot;
                           VInfo.Con_Captured_Slot := Ctx.Vars.Element (PK).Con_Captured_Slot;
                        end if;
                        VInfo.Ada_Type   := PT;
                        VInfo.Pir_Type   := TI.Pir_Type;
                        VInfo.Align      := TI.Align;
                        VInfo.Owner_Subp := Subp_Key;

                        if TI.Kind = TK_Unconstrained_Array
                          or else (TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic)
                        then
                           VInfo.Kind     := VK_Uncon_Array_Param;
                           VInfo.Pir_Name := To_Unbounded_String (PN & "_ptr");
                           VInfo.NDims    := TI.NDims;
                           for D_Idx in 1 .. TI.NDims loop
                              VInfo.First_Regs (D_Idx) := To_Unbounded_String (PN & "_f" & Img (D_Idx));
                              VInfo.Last_Regs (D_Idx)  := To_Unbounded_String (PN & "_l" & Img (D_Idx));
                           end loop;
                           if Subp_Has_Stubs then
                              -- Export array descriptor display so separate subunits can access parent unconstrained array params
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "spda");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spds");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global @__ada_arr_desc = zeroinit, align 8");
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca @__ada_arr_desc, align 8");
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, "memcpy " & Sav & ", " & GA & ", " & Img (Arr_Desc_Size) & ", align 8");
                                 Emit_Store_Arr_Desc (Ctx, GA, PN & "_ptr", VInfo.First_Regs, VInfo.Last_Regs, TI.NDims);
                                 VInfo.Has_Desc    := True;
                                 VInfo.Desc_Global := To_Unbounded_String (GSym);
                                 Ctx.Stub_Restores.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                            Loc_Reg  => To_Unbounded_String (Sav),
                                                            Pir_Type => To_Unbounded_String ("@__ada_arr_desc"),
                                                            Align    => 8,
                                                            Ada_Type => CONST_VOID));
                                 if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                                    declare
                                       FP : constant String := Next_Reg (Ctx.E, "pfp");
                                    begin
                                       Emit_Inst (Ctx.E,
                                         FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                         To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                       Emit_Inst (Ctx.E, "store.ptr " & GA & ", " & FP & ", align 8");
                                    end;
                                 end if;
                              end;
                           elsif VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                              declare
                                 DR : constant String := Next_Reg (Ctx.E, "pdesc");
                                 FP : constant String := Next_Reg (Ctx.E, "pfp");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, DR & " = alloca @__ada_arr_desc, align 8");
                                 Emit_Store_Arr_Desc (Ctx, DR, PN & "_ptr", VInfo.First_Regs, VInfo.Last_Regs, TI.NDims);
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                   To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & DR & ", " & FP & ", align 8");
                                 VInfo.Has_Desc := True;
                              end;
                           end if;
                        elsif TI.Kind = TK_Record or else TI.Kind = TK_Constrained_Array then
                           VInfo.Kind     := VK_Param_Ptr;
                           VInfo.Pir_Name := To_Unbounded_String (PN);
                           if Needs_Constrained_Param (Ctx, PT, Mode) then
                              VInfo.Constrained_Reg := To_Unbounded_String (PN & "_con");
                           end if;
                           if Subp_Has_Stubs then
                              -- Export pointer display so separate subunits can access parent composite formal params
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "sppa");
                                 OldV : constant String := Next_Reg (Ctx.E, "sppo");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spps");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global ptr = zeroinit, align 8");
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca ptr, align 8");
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, OldV & " = load.ptr " & GA & ", align 8");
                                 Emit_Inst (Ctx.E, "store.ptr " & OldV & ", " & Sav & ", align 8");
                                 Emit_Inst (Ctx.E, "store.ptr " & PN & ", " & GA & ", align 8");
                                 Ctx.Stub_Restores.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                            Loc_Reg  => To_Unbounded_String (Sav),
                                                            Pir_Type => To_Unbounded_String ("ptr"),
                                                            Align    => 8,
                                                            Ada_Type => CONST_VOID));
                              end;
                           end if;
                           if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                              declare
                                 FP : constant String := Next_Reg (Ctx.E, "pfp");
                              begin
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                   To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & PN & ", " & FP & ", align 8");
                              end;
                              if VInfo.Con_Captured_Slot > 0 and then Length (VInfo.Constrained_Reg) > 0 then
                                 declare
                                    CSp : constant String := Next_Reg (Ctx.E, "pcs");
                                    CFP : constant String := Next_Reg (Ctx.E, "pcf");
                                 begin
                                    Emit_Entry_Alloca (Ctx.E, CSp & " = alloca i8, align 1");
                                    Emit_Inst (Ctx.E, "store.i8 " & To_String (VInfo.Constrained_Reg) & ", " & CSp & ", align 1");
                                    Emit_Inst (Ctx.E,
                                      CFP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                      To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Con_Captured_Slot));
                                    Emit_Inst (Ctx.E, "store.ptr " & CSp & ", " & CFP & ", align 8");
                                 end;
                              end if;
                           end if;
                        elsif Mode = DN_OUT or else Mode = DN_IN_OUT then
                           -- Scalar OUT / IN OUT: Ada 83 LRM 6.2(7) copy-in / copy-out semantics!
                           if Subp_Has_Stubs then
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "spga");
                                 OldV : constant String := Next_Reg (Ctx.E, "spov");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spsv");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global " & To_String (TI.Pir_Type) & " = zeroinit, align " & Img (TI.Align));
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, OldV & " = load." & To_String (TI.Pir_Type) & " " & GA & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & OldV & ", " & Sav & ", align " & Img (TI.Align));
                                 if Mode = DN_IN_OUT then
                                    declare
                                       CI : constant String := Next_Reg (Ctx.E, "ci");
                                    begin
                                       Emit_Inst (Ctx.E, CI & " = load." & To_String (TI.Pir_Type) & " " & PN & ", align " & Img (TI.Align));
                                       Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & CI & ", " & GA & ", align " & Img (TI.Align));
                                    end;
                                 end if;
                                 if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                                    declare
                                       FP : constant String := Next_Reg (Ctx.E, "pfp");
                                    begin
                                       Emit_Inst (Ctx.E,
                                         FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                         To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                       Emit_Inst (Ctx.E, "store.ptr " & GA & ", " & FP & ", align 8");
                                    end;
                                 end if;
                                 VInfo.Kind     := VK_Global;
                                 VInfo.Pir_Name := To_Unbounded_String (GSym);
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (PN),
                                                        Loc_Reg  => To_Unbounded_String (GA),
                                                        Pir_Type => TI.Pir_Type,
                                                        Align    => TI.Align,
                                                        Ada_Type => PT));
                                 Ctx.Stub_Restores.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                            Loc_Reg  => To_Unbounded_String (Sav),
                                                            Pir_Type => TI.Pir_Type,
                                                            Align    => TI.Align,
                                                            Ada_Type => CONST_VOID));
                              end;
                           else
                              declare
                                 Loc : constant String := Next_Reg (Ctx.E, "io_" & Sanitize_Ident (Sym_Name (PID)) & "_");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, Loc & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 if Mode = DN_IN_OUT then
                                    declare
                                       CI : constant String := Next_Reg (Ctx.E, "ci");
                                    begin
                                       Emit_Inst (Ctx.E, CI & " = load." & To_String (TI.Pir_Type) & " " & PN & ", align " & Img (TI.Align));
                                       Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & CI & ", " & Loc & ", align " & Img (TI.Align));
                                    end;
                                 end if;
                                 if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                                    declare
                                       FP : constant String := Next_Reg (Ctx.E, "pfp");
                                    begin
                                       Emit_Inst (Ctx.E,
                                         FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                         To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                       Emit_Inst (Ctx.E, "store.ptr " & Loc & ", " & FP & ", align 8");
                                    end;
                                 end if;
                                 VInfo.Kind     := VK_Local_Alloca;
                                 VInfo.Pir_Name := To_Unbounded_String (Loc);
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (PN),
                                                        Loc_Reg  => To_Unbounded_String (Loc),
                                                        Pir_Type => TI.Pir_Type,
                                                        Align    => TI.Align,
                                                        Ada_Type => PT));
                              end;
                           end if;
                        else
                           -- Scalar IN parameter
                           if Subp_Has_Stubs then
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "spga");
                                 OldV : constant String := Next_Reg (Ctx.E, "spov");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spsv");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global " & To_String (TI.Pir_Type) & " = zeroinit, align " & Img (TI.Align));
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, OldV & " = load." & To_String (TI.Pir_Type) & " " & GA & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & OldV & ", " & Sav & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & PN & ", " & GA & ", align " & Img (TI.Align));
                                 if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                                    declare
                                       FP : constant String := Next_Reg (Ctx.E, "pfp");
                                    begin
                                       Emit_Inst (Ctx.E,
                                         FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                         To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                       Emit_Inst (Ctx.E, "store.ptr " & GA & ", " & FP & ", align 8");
                                    end;
                                 end if;
                                 VInfo.Kind     := VK_Global;
                                 VInfo.Pir_Name := To_Unbounded_String (GSym);
                                 Ctx.Stub_Restores.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                            Loc_Reg  => To_Unbounded_String (Sav),
                                                            Pir_Type => TI.Pir_Type,
                                                            Align    => TI.Align,
                                                            Ada_Type => CONST_VOID));
                              end;
                           elsif VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                              -- Spill to local alloca so nested subprogram can read via pointer
                              declare
                                 Sp : constant String := Next_Reg (Ctx.E, "pspl");
                                 FP : constant String := Next_Reg (Ctx.E, "pfp");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, Sp & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & PN & ", " & Sp & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                   To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & Sp & ", " & FP & ", align 8");
                                 VInfo.Kind     := VK_Local_Alloca;
                                 VInfo.Pir_Name := To_Unbounded_String (Sp);
                              end;
                           else
                              VInfo.Kind     := VK_Param_Val;
                              VInfo.Pir_Name := To_Unbounded_String (PN);
                           end if;
                        end if;
                        Ctx.Vars.Include (PK, VInfo);
                        Ctx.Vars.Include (Tree_Key (PID), VInfo);
                        Idx := Idx + 1;
                     end;
                  end loop;
               end;
            end loop;
         end;
      end if;
      end;

      -- Mark Secondary Stack at subprogram entry if it uses SS and does not return on SS
      declare
         Subp_SS_Mark : Unbounded_String;
         Need_Stub_Unwind : constant Boolean := not Ctx.Stub_Restores.Is_Empty;
         Stub_Exc_Lbl : Unbounded_String;
      begin
         if not SI.Ret_Uncon_Arr and then Tree_Uses_SS (Ctx, Block_B) then
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", Subp_SS_Mark, Nounwind => True);
            Push_Cleanup (Ctx, CK_SS_Mark, To_String (Subp_SS_Mark));
         end if;

         if Need_Stub_Unwind then
            Stub_Exc_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "subp_sexc"));
            Ctx.Handlers.Append ((Handler_Label => Stub_Exc_Lbl,
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));
         end if;

         -- Emit subprogram block body
         Emit_Block_Body (Ctx, Block_B, Is_Subp => True);

         if Need_Stub_Unwind then
            Ctx.Handlers.Delete_Last;
         end if;

         if not Is_Terminated (Ctx.E) then
            if SI.Is_Function then
               -- Reaching end of function without return raises Program_Error in Ada!
               Emit_Stub_Restores (Ctx);
               Emit_Unwind_To (Ctx, 0, Leave_Masters => False, Release_SS => True, Restore_SP => False);
               Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
            else
               declare
                  N_CO    : constant Natural := Natural (Ctx.Copy_Outs.Length);
                  CO_Vals : array (0 .. Natural'Max (0, N_CO - 1)) of Unbounded_String;
               begin
                  for I in 0 .. N_CO - 1 loop
                     declare
                        CO : constant Copy_Out_Entry := Ctx.Copy_Outs.Element (I);
                        V  : constant String := Next_Reg (Ctx.E, "co");
                     begin
                        Emit_Inst (Ctx.E,
                          V & " = load." & To_String (CO.Pir_Type) & " " &
                          To_String (CO.Loc_Reg) & ", align " & Img (CO.Align));
                        CO_Vals (I) := To_Unbounded_String (V);
                     end;
                  end loop;
                  Emit_Stub_Restores (Ctx);
                  Emit_Unwind_To (Ctx, 0, Leave_Masters => False, Release_SS => True, Restore_SP => False);
                  for I in 0 .. N_CO - 1 loop
                     declare
                        CO : constant Copy_Out_Entry := Ctx.Copy_Outs.Element (I);
                     begin
                        Emit_Range_Check (Ctx, To_String (CO_Vals (I)), To_String (CO.Pir_Type), CO.Ada_Type);
                        Emit_Inst (Ctx.E,
                          "store." & To_String (CO.Pir_Type) & " " & To_String (CO_Vals (I)) &
                          ", " & To_String (CO.Ptr_Reg) & ", align " & Img (CO.Align));
                     end;
                  end loop;
                  Emit_Term (Ctx.E, "ret");
               end;
            end if;
         end if;

         if Need_Stub_Unwind then
            declare
               Sex_R : constant String := Next_Reg (Ctx.E, "ssex");
            begin
               Emit_Label (Ctx.E, To_String (Stub_Exc_Lbl), Sex_R & ": ptr");
               Emit_Stub_Restores (Ctx);
               Emit_Unwind_To (Ctx, 0, Leave_Masters => False, Release_SS => True, Restore_SP => False);
               Emit_Raise_Reg (Ctx, Sex_R);
            end;
         end if;
      end;

      End_Func (Ctx.E, Saved_Buf);

      -- Restore outer subprogram state
      Restore_Subp_State (Ctx, Saved_State);
   end Emit_Subprogram_Body;

   -- =========================================================================
   -- Top-Level Compilation Driver
   -- =========================================================================

   function Package_Has_Compiled_Body
     (PN             : String;
      In_Unit_Bodies : String;
      Lib_Dir        : String := "ADA__LIB") return Boolean
   is
      Up_PN  : constant String := Ada.Strings.Fixed.Translate (PN, Ada.Strings.Maps.Constants.Upper_Case_Map);
      LD     : constant String := (if Lib_Dir'Length > 0 then Lib_Dir else "ADA__LIB");
      Ctl_F  : constant String := LD & ".CTL";
   begin
      if Ada.Strings.Fixed.Index (In_Unit_Bodies, "|" & Up_PN & "|") > 0 then
         return True;
      end if;
      -- Check <Lib_Dir>.CTL for a compiled .BDY file matching Up_PN
      if Ada.Directories.Exists (Ctl_F) then
         declare
            F    : Ada.Text_IO.File_Type;
            Line : String (1 .. 512);
            Last : Natural;
            Short_N : Unbounded_String;
         begin
            Ada.Text_IO.Open (F, Ada.Text_IO.In_File, Ctl_F);
            while not Ada.Text_IO.End_Of_File (F) loop
               Ada.Text_IO.Get_Line (F, Line, Last);
               if Last >= 3 and then Line (1 .. 2) = "U " then
                  Short_N := To_Unbounded_String (Ada.Strings.Fixed.Trim (Line (3 .. Last), Ada.Strings.Both));
                  if not Ada.Text_IO.End_Of_File (F) then
                     Ada.Text_IO.Get_Line (F, Line, Last);
                     declare
                        Full_N : constant String := Ada.Strings.Fixed.Trim (Line (1 .. Last), Ada.Strings.Both);
                     begin
                        if Full_N = Up_PN then
                           Ada.Text_IO.Close (F);
                           return Ada.Directories.Exists (LD & "/" & To_String (Short_N) & ".BDY");
                        end if;
                     end;
                  end if;
               end if;
            end loop;
            Ada.Text_IO.Close (F);
         exception
            when others =>
               if Ada.Text_IO.Is_Open (F) then Ada.Text_IO.Close (F); end if;
         end;
      end if;
      -- Direct 8-char fallback
      declare
         Short_8 : constant String :=
           (if Up_PN'Length <= 8 then Up_PN else Up_PN (Up_PN'First .. Up_PN'First + 7));
      begin
         return Ada.Directories.Exists (LD & "/" & Short_8 & ".BDY");
      end;
   end Package_Has_Compiled_Body;

   procedure Emit_Main_Wrapper
     (Ctx            : in out Context;
      Entry_Sym      : String;
      Comp_Node      : TREE;
      In_Unit_Bodies : String := "|")
   is
      Saved : Pir_Emit.Func_Buffer;
      type Elab_Node is record
         Name      : Unbounded_String; -- UPPERCASE unit name
         Is_Body   : Boolean := False;
         Emit_Call : Boolean := True;
      end record;
      package Elab_Node_Vecs is new Ada.Containers.Vectors (Positive, Elab_Node);
      type Edge_Pair is record
         U : Positive;
         V : Positive; -- V must elaborate BEFORE U
      end record;
      package Edge_Vecs is new Ada.Containers.Vectors (Natural, Edge_Pair);
      Nodes       : Elab_Node_Vecs.Vector;
      Edges       : Edge_Vecs.Vector;
      Visited_CUs : U32_Sets.Set;
      Lib_D       : constant String := To_String (Ctx.Lib_Dir);

      function Has_Edge (U, V : Positive) return Boolean is
      begin
         for E of Edges loop
            if E.U = U and then E.V = V then return True; end if;
         end loop;
         return False;
      end Has_Edge;

      procedure Add_Edge (U, V : Positive) is
      begin
         if U /= V and then not Has_Edge (U, V) then
            Edges.Append ((U => U, V => V));
         end if;
      end Add_Edge;

      function Can_Reach (From_Idx, To_Idx : Positive) return Boolean is
         N_C : constant Natural := Natural (Nodes.Length);
         Vis : array (1 .. Natural'Max (1, N_C)) of Boolean := (others => False);
         function DFS (Cur : Positive) return Boolean is
         begin
            if Cur = To_Idx then
               return True;
            end if;
            Vis (Cur) := True;
            for E of Edges loop
               if E.U = Cur and then not Vis (E.V) then
                  if DFS (E.V) then
                     return True;
                  end if;
               end if;
            end loop;
            return False;
         end DFS;
      begin
         if From_Idx = To_Idx then
            return True;
         end if;
         return DFS (From_Idx);
      end Can_Reach;

      function Is_RTL_Pkg (PN : String) return Boolean is
      begin
         return PN = "_STANDRD" or else PN = "STANDARD" or else PN = "SYSTEM"
           or else PN = "TEXT_IO" or else PN = "IO_EXCEPTIONS" or else PN = "CALENDAR"
           or else PN = "SEQUENTIAL_IO" or else PN = "DIRECT_IO" or else PN = "LOW_LEVEL_IO"
           or else PN = "UNCHECKED_CONVERSION" or else PN = "UNCHECKED_DEALLOCATION";
      end Is_RTL_Pkg;

      function Is_Support_Pkg (PN : String) return Boolean is
      begin
         return PN = "REPORT" or else PN = "FCNDECL" or else PN = "SPPRT13";
      end Is_Support_Pkg;

      function Find_Or_Add (PN : String; Is_Bdy : Boolean; Emit_Call : Boolean := True) return Natural is
      begin
         for I in 1 .. Natural (Nodes.Length) loop
            if Nodes.Element (I).Is_Body = Is_Bdy and then To_String (Nodes.Element (I).Name) = PN then
               if Emit_Call and then not Nodes.Element (I).Emit_Call then
                  declare
                     N_Rec : Elab_Node := Nodes.Element (I);
                  begin
                     N_Rec.Emit_Call := True;
                     Nodes.Replace_Element (I, N_Rec);
                  end;
               end if;
               return I;
            end if;
         end loop;
         Nodes.Append ((Name => To_Unbounded_String (PN), Is_Body => Is_Bdy, Emit_Call => Emit_Call));
         return Natural (Nodes.Length);
      end Find_Or_Add;

      function Find_Node (PN : String; Is_Bdy : Boolean) return Natural is
      begin
         for I in 1 .. Natural (Nodes.Length) loop
            if Nodes.Element (I).Is_Body = Is_Bdy and then To_String (Nodes.Element (I).Name) = PN then
               return I;
            end if;
         end loop;
         return 0;
      end Find_Node;

      function Root_Parent_Name (N : TREE) return String is
         Cur : TREE := N;
      begin
         for Step in 1 .. 16 loop
            exit when Is_Void_Tree (Cur);
            if KIND (Cur) = DN_SELECTED then
               Cur := D (AS_NAME, Cur);
            else
               return Ada.Strings.Fixed.Translate (Sym_Name (Cur), Ada.Strings.Maps.Constants.Upper_Case_Map);
            end if;
         end loop;
         return "";
      end Root_Parent_Name;

      procedure Register_Pkg (PN : String; Emit_Call : Boolean := True; Has_Body_Node : Boolean := False) is
         SI, BI : Natural;
      begin
         if PN'Length = 0 or else Is_RTL_Pkg (PN) then
            return;
         end if;
         SI := Find_Or_Add (PN, False, Emit_Call => Emit_Call);
         if (Emit_Call and then Package_Has_Compiled_Body (PN, In_Unit_Bodies, Lib_D))
           or else Has_Body_Node
         then
            BI := Find_Or_Add (PN, True, Emit_Call => Emit_Call and then Package_Has_Compiled_Body (PN, In_Unit_Bodies, Lib_D));
            if SI > 0 and then BI > 0 then
               Add_Edge (BI, SI);
            end if;
         end if;
      end Register_Pkg;

      procedure Add_Context_Edges (CU : TREE; U_Idx : Natural);

      procedure Collect_Withs_From_CU (CU : TREE) is
         WL : SEQ_TYPE := LIST (CU);
         TW : TREE;
      begin
         while not IS_EMPTY (WL) loop
            POP (WL, TW);
            declare
               WCU : constant TREE := D (TW_COMP_UNIT, TW);
               WAD : constant TREE := (if not Is_Void_Tree (WCU) then D (AS_ALL_DECL, WCU) else CONST_VOID);
            begin
               if not Is_Void_Tree (WAD) then
                  if KIND (WAD) in DN_PACKAGE_DECL | DN_GENERIC_DECL then
                     declare
                        WPN : constant String :=
                          Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, WAD)), Ada.Strings.Maps.Constants.Upper_Case_Map);
                     begin
                        Register_Pkg (WPN, Emit_Call => True);
                     end;
                  elsif KIND (WAD) = DN_SUBPROG_ENTRY_DECL then
                     declare
                        WPN : constant String :=
                          Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, WAD)), Ada.Strings.Maps.Constants.Upper_Case_Map);
                        Is_Inst : constant Boolean :=
                          not Is_Void_Tree (D (AS_UNIT_KIND, WAD)) and then KIND (D (AS_UNIT_KIND, WAD)) = DN_INSTANTIATION;
                     begin
                        Register_Pkg (WPN, Emit_Call => Is_Inst);
                     end;
                  elsif KIND (WAD) = DN_SUBPROGRAM_BODY then
                     declare
                        SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, WAD));
                        WPN : constant String :=
                          Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                     begin
                        Register_Pkg (WPN, Emit_Call => (KIND (SID) = DN_GENERIC_ID), Has_Body_Node => True);
                     end;
                  end if;
               end if;
            end;
         end loop;
      end Collect_Withs_From_CU;

      procedure Add_Context_Edges (CU : TREE; U_Idx : Natural) is
         procedure Scan_List (Seq_Node : TREE) is
            CL : SEQ_TYPE;
            CE : TREE;
         begin
            if Is_Void_Tree (Seq_Node) then
               return;
            end if;
            CL := LIST (Seq_Node);
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
                           WN : constant String :=
                             Ada.Strings.Fixed.Translate (Sym_Name (NM), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           VI : constant Natural := Find_Node (WN, False);
                        begin
                           if VI > 0 and then VI /= U_Idx then
                              Add_Edge (U_Idx, VI);
                           end if;
                        end;
                     end loop;
                  end;
                  Scan_List (D (AS_USE_PRAGMA_S, CE));
               elsif KIND (CE) = DN_PRAGMA then
                  declare
                     PN_Str : constant String :=
                       Ada.Strings.Fixed.Translate (Sym_Name (D (AS_USED_NAME_ID, CE)), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  begin
                     if PN_Str = "ELABORATE"
                       and then not Is_Void_Tree (D (AS_USED_NAME_ID, CE))
                       and then not Is_Void_Tree (D (SM_DEFN, D (AS_USED_NAME_ID, CE)))
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
                                 EN  : constant String :=
                                   Ada.Strings.Fixed.Translate (Sym_Name (Exp), Ada.Strings.Maps.Constants.Upper_Case_Map);
                                 VS  : constant Natural := Find_Node (EN, False);
                                 VB  : constant Natural := Find_Node (EN, True);
                              begin
                                 if VS > 0 and then VS /= U_Idx then Add_Edge (U_Idx, VS); end if;
                                 if VB > 0 and then VB /= U_Idx then Add_Edge (U_Idx, VB); end if;
                              end;
                           end loop;
                        end;
                     end if;
                  end;
               end if;
            end loop;
         end Scan_List;
      begin
         if U_Idx = 0 then
            return;
         end if;
         Scan_List (D (AS_CONTEXT_ELEM_S, CU));
      end Add_Context_Edges;

      procedure Add_Ext_With_Edges (CU : TREE) is
         WL : SEQ_TYPE := LIST (CU);
         TW : TREE;
      begin
         while not IS_EMPTY (WL) loop
            POP (WL, TW);
            declare
               WCU : constant TREE := D (TW_COMP_UNIT, TW);
               WAD : constant TREE := (if not Is_Void_Tree (WCU) then D (AS_ALL_DECL, WCU) else CONST_VOID);
            begin
               if not Is_Void_Tree (WCU) and then not Is_Void_Tree (WAD)
                 and then not Visited_CUs.Contains (Tree_Key (WCU))
               then
                  Visited_CUs.Include (Tree_Key (WCU));
                  if KIND (WAD) in DN_PACKAGE_DECL | DN_GENERIC_DECL | DN_SUBPROG_ENTRY_DECL then
                     Add_Context_Edges (WCU, Find_Node (Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, WAD)), Ada.Strings.Maps.Constants.Upper_Case_Map), False));
                  elsif KIND (WAD) in DN_PACKAGE_BODY | DN_SUBPROGRAM_BODY then
                     Add_Context_Edges (WCU, Find_Node (Ada.Strings.Fixed.Translate (Sym_Name (Canonical_Def (D (AS_SOURCE_NAME, WAD))), Ada.Strings.Maps.Constants.Upper_Case_Map), True));
                  end if;
               end if;
            end;
         end loop;
      end Add_Ext_With_Edges;

      L1 : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      CU : TREE;
   begin
      Begin_Func (Ctx.E, Saved, "export func @main() -> i32 cc ""c""");

      -- Pass 1: discover all non-RTL library units across ALL CUs (specs, bodies, subprograms, subunits, and their WITHs)
      while not IS_EMPTY (L1) loop
         POP (L1, CU);
         Collect_Withs_From_CU (CU);
         declare
            AD : constant TREE := D (AS_ALL_DECL, CU);
         begin
            if not Is_Void_Tree (AD) then
               if KIND (AD) = DN_PACKAGE_DECL or else KIND (AD) = DN_GENERIC_DECL then
                  Register_Pkg (Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, AD)), Ada.Strings.Maps.Constants.Upper_Case_Map), Emit_Call => True);
               elsif KIND (AD) = DN_SUBPROG_ENTRY_DECL then
                  declare
                     Is_Inst : constant Boolean :=
                       not Is_Void_Tree (D (AS_UNIT_KIND, AD)) and then KIND (D (AS_UNIT_KIND, AD)) = DN_INSTANTIATION;
                  begin
                     Register_Pkg (Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, AD)), Ada.Strings.Maps.Constants.Upper_Case_Map), Emit_Call => Is_Inst);
                  end;
               elsif KIND (AD) = DN_SUBPROGRAM_BODY then
                  declare
                     SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                  begin
                     Register_Pkg (Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map),
                       Emit_Call => (KIND (SID) = DN_GENERIC_ID), Has_Body_Node => True);
                  end;
               end if;
            end if;
         end;
      end loop;

      -- Pass 2: add WITH and PRAGMA ELABORATE edges from every CU (in-file and external WITHed CUs)
      L1 := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      while not IS_EMPTY (L1) loop
         POP (L1, CU);
         Add_Ext_With_Edges (CU);
         declare
            AD : constant TREE := D (AS_ALL_DECL, CU);
         begin
            if not Is_Void_Tree (AD) then
               if KIND (AD) in DN_PACKAGE_DECL | DN_GENERIC_DECL | DN_SUBPROG_ENTRY_DECL then
                  Add_Context_Edges (CU, Find_Node (Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, AD)), Ada.Strings.Maps.Constants.Upper_Case_Map), False));
               elsif KIND (AD) in DN_PACKAGE_BODY | DN_SUBPROGRAM_BODY then
                  Add_Context_Edges (CU, Find_Node (Ada.Strings.Fixed.Translate (Sym_Name (Canonical_Def (D (AS_SOURCE_NAME, AD))), Ada.Strings.Maps.Constants.Upper_Case_Map), True));
               elsif KIND (AD) = DN_SUBUNIT then
                  declare
                     Root_N : constant String := Root_Parent_Name (D (AS_NAME, AD));
                     BI     : constant Natural := Find_Node (Root_N, True);
                  begin
                     if BI > 0 then
                        Add_Context_Edges (CU, BI);
                     end if;
                  end;
               end if;
            end if;
         end;
      end loop;

      -- Support packages (REPORT, FCNDECL, SPPRT13) elaborate before user packages when unconstrained (transitively!)
      declare
         N_Count : constant Natural := Natural (Nodes.Length);
      begin
         for U in 1 .. N_Count loop
            if not Is_Support_Pkg (To_String (Nodes.Element (U).Name)) then
               for V in 1 .. N_Count loop
                  if Is_Support_Pkg (To_String (Nodes.Element (V).Name)) and then not Can_Reach (V, U) then
                     Add_Edge (U, V);
                  end if;
               end loop;
            end if;
         end loop;

         -- Topological sort (Kahn's: support first, specs before bodies, original order)
         declare
            Done    : array (1 .. N_Count) of Boolean := (others => False);
            Emitted : Natural := 0;
            Step_No : Natural := 0;
         begin
            while Emitted < N_Count loop
               declare
                  Pick : Natural := 0;
                  function Node_Prio (I : Natural) return Natural is
                     PN : constant String := To_String (Nodes.Element (I).Name);
                     R  : Natural := (if PN = "REPORT" then 0 elsif Is_Support_Pkg (PN) then 10 else 20);
                  begin
                     if Nodes.Element (I).Is_Body then R := R + 5; end if;
                     return R;
                  end Node_Prio;
               begin
                  for I in 1 .. N_Count loop
                     if not Done (I) then
                        declare
                           Ready : Boolean := True;
                        begin
                           for E of Edges loop
                              if E.U = I and then not Done (E.V) then
                                 Ready := False;
                                 exit;
                              end if;
                           end loop;
                           if Ready then
                              if Pick = 0 or else Node_Prio (I) < Node_Prio (Pick) then
                                 Pick := I;
                              end if;
                           end if;
                        end;
                     end if;
                  end loop;
                  if Pick = 0 then
                     -- P3-5.15: Reject circular elaboration dependencies (LRM 10.5(2, 11))!
                     Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "elaboration error: circular elaboration dependency detected");
                     raise Elab_Cycle_Error;
                  end if;
                  exit when Pick = 0;
                  Done (Pick) := True;
                  Emitted := Emitted + 1;
                  if Nodes.Element (Pick).Emit_Call then
                     Step_No := Step_No + 1;
                     declare
                        PN_Low : constant String := Ada_Sym_Util.Sanitize_Ada_Ident (To_String (Nodes.Element (Pick).Name));
                        Sym    : constant String := "@ada__" & PN_Low & (if Nodes.Element (Pick).Is_Body then "__elab_body" else "__elab_spec");
                        NxtL   : constant String := ".el_" & Img (Step_No);
                     begin
                        Add_Extern_Func (Ctx.E, Sym, "extern func " & Sym & "() -> void cc ""internal""");
                        Emit_Term (Ctx.E, "invoke " & Sym & "() then " & NxtL & " unwind .unhandled(exn)");
                        Emit_Label (Ctx.E, NxtL);
                     end;
                  end if;
               end;
            end loop;
         end;
      end;

      -- Activate any library-level tasks before entering main, and wait for them on exit!
      Emit_Inst (Ctx.E, "%__gm = call @__ada_global_master()");
      Emit_Term (Ctx.E, "invoke @__ada_master_activate(%__gm) then .run_main unwind .unhandled(exn)");
      Emit_Label (Ctx.E, ".run_main");
      -- Invoke Ada main procedure with top-level unhandled-exception handler
      Emit_Term (Ctx.E, "invoke " & Entry_Sym & "() then .done unwind .unhandled(exn)");
      Emit_Label (Ctx.E, ".done");
      Emit_Inst (Ctx.E, "call @__ada_master_leave(%__gm)");
      Emit_Term (Ctx.E, "ret 0");
      Emit_Label (Ctx.E, ".unhandled", "%exn: ptr");
      Emit_Inst (Ctx.E, "call @__ada_unhandled_exception(%exn)");
      Emit_Term (Ctx.E, "unreachable");

      End_Func (Ctx.E, Saved);
   end Emit_Main_Wrapper;

   procedure Emit_Task_Body
     (Ctx       : in out Context;
      Body_Node : TREE)
   is
      TB_Id   : constant TREE := D (AS_SOURCE_NAME, Body_Node);
      TS      : constant TREE := (if not Is_Void_Tree (TB_Id) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TB_Id))) else CONST_VOID);
      Block_B : TREE := D (AS_BODY, Body_Node);
      TK      : Unsigned_32;
      TI      : Task_Info;

      Saved_Buf      : Pir_Emit.Func_Buffer;
      Saved_State    : constant Subp_State_Snapshot := Save_Subp_State (Ctx);
      Saved_Key      : constant Unsigned_32 := Saved_State.Cur_Subp_Key;

      Captured_Vars  : Cap_Vecs.Vector;
      Nested_Subps   : U32_Vecs.Vector;
   begin
      if Is_Void_Tree (TS) or else KIND (TS) /= DN_TASK_SPEC then
         return;
      end if;
      Register_Task_Spec (Ctx, TS, Saved_Key);
      TK := Tree_Key (TS);
      TI := Ctx.Tasks.Element (TK);

      if Is_Void_Tree (Block_B) or else KIND (Block_B) = DN_STUB then
         declare
            Orig_TS : constant TREE :=
              (if not Is_Void_Tree (TB_Id)
               then Resolve_Type_Spec (D (SM_TYPE_SPEC, TB_Id))
               else CONST_VOID);
            Sub_Found : Boolean := False;
         begin
            if not Is_Void_Tree (Orig_TS) and then Ctx.Gen_Subunits.Contains (Tree_Key (Orig_TS)) then
               Block_B := D (AS_BODY, Ctx.Gen_Subunits.Element (Tree_Key (Orig_TS)));
               Sub_Found := True;
            elsif Ctx.Gen_Subunits.Contains (TK) then
               Block_B := D (AS_BODY, Ctx.Gen_Subunits.Element (TK));
               Sub_Found := True;
            end if;
            if not Sub_Found then
               Add_Extern_Func (Ctx.E, To_String (TI.Body_Fn),
                 "extern func " & To_String (TI.Body_Fn) & "(%__sl: ptr, %__self: ptr) -> void cc ""c""");
               return;
            end if;
         end;
      end if;

      declare
         TSI : Subp_Info;
      begin
         if Ctx.Subps.Contains (TK) then
            TSI := Ctx.Subps.Element (TK);
         end if;
         TSI.Pir_Name    := TI.Body_Fn;
         TSI.Parent_Subp := TI.Parent_Subp;
         TSI.Needs_SL    := True;
         Ctx.Subps.Include (TK, TSI);
      end;

      declare
         Temp_Owners : Var_Maps.Map;
         Local_Meta  : U32_Vecs.Vector;
         procedure Collect_Task_Owners (Node : TREE; In_Gen_Bdy : Boolean := False) is
            K : NODE_NAME;
         begin
            if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then return; end if;
            K := KIND (Node);
            if K = DN_SUBPROGRAM_BODY or else K = DN_PACKAGE_BODY then
               declare
                  SN : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
               begin
                  if not Is_Void_Tree (SN) and then KIND (SN) = DN_GENERIC_ID then
                     if not Ctx.Generic_Body.Contains (Tree_Key (SN))
                       or else Is_Void_Tree (D (AS_BODY, Node))
                       or else KIND (D (AS_BODY, Node)) /= DN_STUB
                     then
                        Ctx.Generic_Body.Include (Tree_Key (SN), Node);
                     end if;
                  end if;
               end;
               if K = DN_SUBPROGRAM_BODY then return; end if;
               if K = DN_PACKAGE_BODY then
                  declare
                     Sub_P : constant TREE := Find_Gen_Subunit_Body (Ctx, Node);
                  begin
                     if not Is_Void_Tree (Sub_P) then
                        Collect_Task_Owners (D (AS_BODY, Sub_P), In_Gen_Bdy);
                        return;
                     end if;
                  end;
               end if;
            end if;
            if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then return; end if;
            if K = DN_TYPE_DECL then
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS_A : constant TREE :=
                    (if not Is_Void_Tree (TID) and then KIND (TID) in CLASS_TYPE_NAME
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TID))) else CONST_VOID);
               begin
                  if not Is_Void_Tree (TS_A) and then KIND (TS_A) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                    and then Type_Contains_Task (Ctx, D (SM_DESIG_TYPE, TS_A))
                  then
                     declare
                        BS : constant TREE := Base_Type_Spec (TS_A);
                     begin
                        if not Is_Void_Tree (BS) and then KIND (BS) = DN_ACCESS and then Is_Void_Tree (D (SM_DERIVED, BS)) then
                           Local_Meta.Append (Tree_Key (BS));
                        end if;
                     end;
                  end if;
               end;
               return;
            elsif K = DN_VARIABLE_DECL or else K = DN_CONSTANT_DECL then
               declare
                  NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Node));
                  ID : TREE;
               begin
                  while not IS_EMPTY (NS) loop
                     POP (NS, ID);
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := TK;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                     end;
                  end loop;
               end;
               return;
            elsif K = DN_TASK_DECL then
               declare
                  Src : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS_M : TREE := CONST_VOID;
               begin
                  if not Is_Void_Tree (Src) and then KIND (Src) = DN_VARIABLE_ID then
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := TK;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (Src)), V);
                     end;
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src)));
                  elsif not Is_Void_Tree (Src) and then KIND (Src) in CLASS_TYPE_NAME then
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Src)));
                  end if;
                  if not Is_Void_Tree (TS_M) and then KIND (TS_M) = DN_TASK_SPEC then
                     Local_Meta.Append (Tree_Key (TS_M));
                  end if;
               end;
               return;
            elsif K = DN_LENGTH_ENUM_REP then
               -- P3-6.6: Capture local dynamic 'STORAGE_SIZE slots in parent static-link frame
               declare
                  AN : constant TREE := D (AS_NAME, Node);
                  EN : constant TREE := D (AS_EXP, Node);
               begin
                  if not Is_Void_Tree (AN) and then KIND (AN) = DN_ATTRIBUTE and then not Is_Void_Tree (EN)
                    and then not Is_Void_Tree (D (AS_USED_NAME_ID, AN))
                    and then Sym_Name (D (AS_USED_NAME_ID, AN)) = "STORAGE_SIZE"
                  then
                     Local_Meta.Append (Tree_Key (EN));
                  end if;
               end;
               return;
            elsif K = DN_RENAMES_OBJ_DECL then
               declare
                  ID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := TK;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
               return;
            elsif K = DN_FOR or else K = DN_REVERSE then
               declare
                  ID : constant TREE := D (AS_SOURCE_NAME, Node);
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := TK;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
            elsif K = DN_SUBPROG_ENTRY_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     Collect_Task_Owners (D (SM_DECL_S, UK), In_Gen_Bdy);
                     return;
                  elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                     declare
                        SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        V   : Var_Info;
                     begin
                        V.Owner_Subp    := TK;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (SID), V);
                     end;
                     return;
                  end if;
               end;
            elsif K = DN_PACKAGE_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     declare
                        PID    : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                        GK     : constant Unsigned_32 := Tree_Key (Gen_Id);
                        GB     : TREE := CONST_VOID;
                     begin
                        Collect_Task_Owners (D (SM_DECL_S, UK), In_Gen_Bdy);
                        Collect_Task_Owners (D (SM_SPEC, PID), In_Gen_Bdy);
                        -- P3-5.1: Propagate nested generic bodies in instantiated package spec
                        declare
                           GSp : constant TREE := (if not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_SPEC, Gen_Id) else CONST_VOID);
                           ISp : constant TREE := D (SM_SPEC, PID);
                           Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
                        begin
                           if not Is_Void_Tree (GSp) and then KIND (GSp) = DN_PACKAGE_SPEC
                             and then not Is_Void_Tree (ISp) and then KIND (ISp) = DN_PACKAGE_SPEC
                           then
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, GSp), D (AS_DECL_S1, ISp));
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, GSp), D (AS_DECL_S2, ISp));
                           end if;
                           Ctx.Generic_Subst := Saved_Sub;
                        end;
                        if Ctx.Generic_Body.Contains (GK) then
                           GB := Ctx.Generic_Body.Element (GK);
                        elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then
                           GB := D (SM_BODY, Gen_Id);
                           if Is_Void_Tree (GB) then
                              GB := D (XD_BODY, Gen_Id);
                           end if;
                        end if;
                        if not Is_Void_Tree (GB) then
                           if KIND (GB) = DN_PACKAGE_BODY then
                              Collect_Task_Owners (D (AS_BODY, GB), True);
                           else
                              Collect_Task_Owners (GB, True);
                           end if;
                        end if;
                        return;
                     end;
                  end if;
               end;
            elsif K = DN_ACCEPT then
               declare
                  APS : constant TREE := D (AS_PARAM_S, Node);
               begin
                  if not Is_Void_Tree (APS) then
                     declare
                        PL : SEQ_TYPE := LIST (APS);
                        P  : TREE;
                     begin
                        while not IS_EMPTY (PL) loop
                           POP (PL, P);
                           declare
                              NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                              ID : TREE;
                           begin
                              while not IS_EMPTY (NS) loop
                                 POP (NS, ID);
                                 declare
                                    V : Var_Info;
                                 begin
                                    V.Owner_Subp    := TK;
                                    V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                                    Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                                    Temp_Owners.Include (Tree_Key (ID), V);
                                 end;
                              end loop;
                           end;
                        end loop;
                     end;
                  end if;
               end;
            end if;
            if ARITY (Node) = ARBITRARY then
               declare
                  L : SEQ_TYPE := LIST (Node);
                  It : TREE;
               begin
                  while not IS_EMPTY (L) loop POP (L, It); Collect_Task_Owners (It, In_Gen_Bdy); end loop;
               end;
            else
               for I in 1 .. ARITIES'Pos (ARITY (Node)) loop Collect_Task_Owners (DABS (I, Node), In_Gen_Bdy); end loop;
            end if;
         end Collect_Task_Owners;
      begin
         Collect_Task_Owners (Block_B);
         Scan_Tree_For_Captures (Ctx, Block_B, TK, Temp_Owners, Ctx.Subps, Captured_Vars, Nested_Subps, TK);
         if not Local_Meta.Is_Empty and then (not Nested_Subps.Is_Empty or else Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Block_B))) then
            for MK of Local_Meta loop
               declare
                  Found_M : Boolean := False;
               begin
                  for CP of Captured_Vars loop
                     if CP.Inst_Key = 0 and then CP.Var_Key = MK then
                        Found_M := True;
                        exit;
                     end if;
                  end loop;
                  if not Found_M then
                     Captured_Vars.Append ((Inst_Key => 0, Var_Key => MK));
                  end if;
               end;
            end loop;
         end if;
      end;

      declare
         TSI : Subp_Info := Ctx.Subps.Element (TK);
      begin
         for NK of Nested_Subps loop
            if Ctx.Subps.Contains (NK) then
               declare
                  NSI : Subp_Info := Ctx.Subps.Element (NK);
               begin
                  NSI.Needs_SL := True;
                  Ctx.Subps.Replace (NK, NSI);
               end;
            end if;
         end loop;
         if not Captured_Vars.Is_Empty or else not Nested_Subps.Is_Empty
           or else Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Block_B))
         then
            TSI.Has_Frame := True;
            TSI.Frame_Type := To_Unbounded_String ("@__frame_" & Tree_Key_Str (TS) & To_String (Ctx.Gen_Inst_Tag));
            declare
               -- Slot 0 = parent static link (%__sl), Slot 1 = this task's %__self,
               -- Slots 2..N_Cap+1 = captured vars, Slots N_Cap+2..2*N_Cap+1 = captured 'CONSTRAINED flags
               N_Cap   : constant Natural := Natural (Captured_Vars.Length);
               FT_Decl : Unbounded_String := To_Unbounded_String ("type " & To_String (TSI.Frame_Type) & " = { ptr, ptr");
            begin
               for Slot in 1 .. N_Cap loop
                  Append (FT_Decl, ", ptr");
               end loop;
               for Slot in 1 .. N_Cap loop
                  Append (FT_Decl, ", ptr");
                  declare
                     CP : constant Cap_Pair := Captured_Vars.Element (Slot - 1);
                     CK : constant Unsigned_32 := CP.Var_Key;
                  begin
                     if CP.Inst_Key /= 0 then
                        Ctx.Gen_Cap_Slots.Include
                          (Shift_Left (Unsigned_64 (CP.Inst_Key), 32) or Unsigned_64 (CK),
                           (Cap_Slot => Slot + 1, Con_Cap_Slot => N_Cap + Slot + 1));
                     else
                        declare
                           V : Var_Info;
                        begin
                           if Ctx.Vars.Contains (CK) then V := Ctx.Vars.Element (CK); end if;
                           V.Captured_Slot     := Slot + 1;
                           V.Con_Captured_Slot := N_Cap + Slot + 1;
                           V.Owner_Subp        := TK;
                           Ctx.Vars.Include (CK, V);
                        end;
                     end if;
                  end;
               end loop;
               Append (FT_Decl, " }");
               Add_Type_Decl (Ctx.E, To_String (FT_Decl));
            end;
            Ctx.Subps.Replace (TK, TSI);
         end if;

         declare
            TS_Src : constant TREE := D (XD_SOURCE_NAME, TS);
            Link   : constant String :=
              (if Is_Library_Level (TB_Id)
                 or else (not Is_Void_Tree (TS_Src) and then Is_Library_Level (TS_Src))
                 or else Ctx.In_Subunit
               then "export " else "");
            Hdr  : constant String := Link & "func " & To_String (TI.Body_Fn) & "(%__sl: ptr, %__self: ptr) -> void cc ""c""";
         begin
            Begin_Func (Ctx.E, Saved_Buf, Hdr);
         end;

         Ctx.Cur_Subp_Key      := TK;
         Ctx.Cur_Subp_Node     := Body_Node;
         Ctx.Cur_Is_Func       := False;
         Ctx.Cur_Ret_Type      := CONST_VOID;
         Ctx.Cur_Ret_Comp      := False;
         Ctx.Cur_Ret_Uncon     := False;
         Ctx.Cur_Ret_Ptr       := Null_Unbounded_String;
         Ctx.Cur_SL_Reg        := To_Unbounded_String ("%__sl");
         Ctx.Cur_Frame_Reg     := Null_Unbounded_String;
         Reset_Subp_State (Ctx);
         Ctx.Cur_Task_Reg      := To_Unbounded_String ("%__self");
         Ctx.Cur_Task_Exit_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "task_exit"));

         if TSI.Has_Frame then
            declare
               FR  : constant String := Next_Reg (Ctx.E, "tfr");
               FP  : constant String := Next_Reg (Ctx.E, "tfrp");
               FP1 : constant String := Next_Reg (Ctx.E, "tfrs");
            begin
               Emit_Entry_Alloca (Ctx.E, FR & " = alloca " & To_String (TSI.Frame_Type) & ", align 8");
               Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (TSI.Frame_Type) & ", " & FR & ", 0");
               Emit_Inst (Ctx.E, "store.ptr %__sl, " & FP & ", align 8");
               Emit_Inst (Ctx.E, FP1 & " = field_ptr " & To_String (TSI.Frame_Type) & ", " & FR & ", 1");
               Emit_Inst (Ctx.E, "store.ptr %__self, " & FP1 & ", align 8");
               Ctx.Cur_Frame_Reg := To_Unbounded_String (FR);
            end;
         end if;

         declare
            Item_S    : constant TREE := D (AS_ITEM_S, Block_B);
            Stm_S     : constant TREE := D (AS_STM_S, Block_B);
            Alt_S     : constant TREE := D (AS_ALTERNATIVE_S, Block_B);
            Has_Hdl   : constant Boolean := (not Is_Void_Tree (Alt_S)) and then (not IS_EMPTY (LIST (Alt_S)));
            Has_Tasks : constant Boolean := Item_List_Has_Tasks (Ctx, Item_S);
            Act_Fail  : constant String := Next_Label (Ctx.E, "tact_f");
            Top_Exc   : constant String := Next_Label (Ctx.E, "texc_top");
            Exn_Act   : constant String := Next_Reg (Ctx.E, "taex");
            Exn_Top   : constant String := Next_Reg (Ctx.E, "ttex");
            Local_Mst : Unbounded_String;
            Mst_Slot  : Unbounded_String;
            Has_Dyn   : Boolean := False;
            Dummy     : Unbounded_String;
         begin
            if Has_Tasks then
               declare
                  MS : constant String := Next_Reg (Ctx.E, "tmst");
               begin
                  Emit_Entry_Alloca (Ctx.E, MS & " = alloca ptr, align 8");
                  Emit_Inst (Ctx.E, "store.ptr 0, " & MS & ", align 8");
                  Mst_Slot := To_Unbounded_String (MS);
               end;
            end if;
            Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (Act_Fail),
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));
            if Has_Tasks then
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_enter", "", "ptr", Local_Mst);
               Emit_Inst (Ctx.E, "store.ptr " & To_String (Local_Mst) & ", " & To_String (Mst_Slot) & ", align 8");
               Ctx.Cur_Master_Reg := Local_Mst;
               Push_Cleanup (Ctx, CK_Master, To_String (Local_Mst));
               Ctx.Cur_Task_Base_Cleanup_Depth := Cleanup_Depth (Ctx);
               Scan_Label_Cleanup_Depths (Ctx, Stm_S, Ctx.Cur_Task_Base_Cleanup_Depth);
               Scan_Label_Cleanup_Depths (Ctx, Alt_S, Ctx.Cur_Task_Base_Cleanup_Depth);
            else
               Ctx.Cur_Task_Base_Cleanup_Depth := 0;
               Scan_Label_Cleanup_Depths (Ctx, Stm_S, 0);
               Scan_Label_Cleanup_Depths (Ctx, Alt_S, 0);
            end if;
            -- P3-5.10 & P3-5.17: Export task-body declarations & exceptions when task body contains separate subunits!
            declare
               Task_Has_Stubs  : Boolean := False;
               Saved_Has_Stubs : constant Boolean := Ctx.Has_Stubs;
            begin
               if not Is_Void_Tree (Item_S) then
                  declare
                     SL_T : SEQ_TYPE := LIST (Item_S);
                     IT_T : TREE;
                  begin
                     while not IS_EMPTY (SL_T) loop
                        POP (SL_T, IT_T);
                        if (KIND (IT_T) = DN_SUBPROGRAM_BODY or else KIND (IT_T) = DN_PACKAGE_BODY or else KIND (IT_T) = DN_TASK_BODY)
                          and then not Is_Void_Tree (D (AS_BODY, IT_T))
                          and then KIND (D (AS_BODY, IT_T)) = DN_STUB
                          and then not Is_Gen_Subunit_Stub (Ctx, IT_T)
                        then
                           Task_Has_Stubs := True;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;
               if Task_Has_Stubs then
                  Ctx.Has_Stubs := True;
               end if;
               Emit_Decl_Items (Ctx, Item_S, Is_Global => Task_Has_Stubs, Export_All => Task_Has_Stubs, Has_Dyn => Has_Dyn);
               Ctx.Has_Stubs := Saved_Has_Stubs;
            end;
            Ctx.Handlers.Delete_Last;

            Emit_Call_Or_Invoke (Ctx, "@__ada_task_activated", "%__self", "", Dummy, Nounwind => True);

            Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (Top_Exc),
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));

            if Has_Hdl then
               declare
                  Hdl_Lbl   : constant String := Next_Label (Ctx.E, "exc_h");
                  After_Lbl : constant String := Next_Label (Ctx.E, "exc_end");
                  H_SP      : constant String := Next_Reg (Ctx.E, "thsp");
               begin
                  Emit_Inst (Ctx.E, H_SP & " = stacksave");
                  Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (Hdl_Lbl),
                                        Current_Exn   => Null_Unbounded_String,
                                        In_Handler    => False,
                                        SS_Mark       => Null_Unbounded_String,
                                        SP_Mark       => To_Unbounded_String (H_SP)));
                  if Has_Tasks then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate", To_String (Local_Mst), "", Dummy);
                  end if;
                  Emit_Stm_List (Ctx, Stm_S);
                  if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & After_Lbl); end if;
                  Emit_Exception_Handlers (Ctx, Alt_S, Hdl_Lbl, After_Lbl);
                  Ctx.Handlers.Delete_Last;
                  Emit_Label (Ctx.E, After_Lbl);
               end;
            else
               if Has_Tasks then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate", To_String (Local_Mst), "", Dummy);
               end if;
               Emit_Stm_List (Ctx, Stm_S);
            end if;

            Ctx.Handlers.Delete_Last;
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));
            end if;

            Emit_Label (Ctx.E, Act_Fail, Exn_Act & ": ptr");
            -- Notify activator and set T'CALLABLE=FALSE first so we cannot deadlock with
            -- already-activated child tasks waiting on T or T's activator (LRM 9.3(5))
            Emit_Call_Or_Invoke (Ctx, "@__ada_task_activation_failed", "%__self", "", Dummy, Nounwind => True);
            if Has_Tasks then
               declare
                  ML : constant String := Next_Reg (Ctx.E, "tml");
               begin
                  Emit_Inst (Ctx.E, ML & " = load.ptr " & To_String (Mst_Slot) & ", align 8");
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_unactivated", ML, "", Dummy, Nounwind => True);
               end;
            end if;
            Emit_Term (Ctx.E, "ret");

            Emit_Label (Ctx.E, Top_Exc, Exn_Top & ": ptr");
            Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));

            Emit_Label (Ctx.E, To_String (Ctx.Cur_Task_Exit_Lbl));
            if Has_Tasks then
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_completed", "%__self", "", Dummy, Nounwind => True);
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (Local_Mst), "", Dummy);
            end if;
            Emit_Term (Ctx.E, "ret");
         end;

         End_Func (Ctx.E, Saved_Buf);

         Restore_Subp_State (Ctx, Saved_State);
      end;
   end Emit_Task_Body;

   procedure Emit_Compilation (Ctx : in out Context; Comp_Node : TREE; Only_CU : Natural := 0) is
      CU_List        : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      CU             : TREE;
      In_Unit_Bodies : Unbounded_String := To_Unbounded_String ("|");
   begin
      Ensure_Runtime_Externs (Ctx);

      -- Pre-scan compilation units in this file to record bodies, generics, latest CU per unit, and main CU
      declare
         Scan_L : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
         SCU    : TREE;
         CU_Idx : Natural := 0;
         Main_CU_Idx : Natural := 0;
         type Unit_Latest is record
            Key_Str : Unbounded_String;
            Idx     : Natural := 0;
         end record;
         package UL_Vecs is new Ada.Containers.Vectors (Positive, Unit_Latest);
         Latest : UL_Vecs.Vector;
         procedure Set_Latest (KStr : String; Idx : Natural) is
         begin
            for I in 1 .. Natural (Latest.Length) loop
               if To_String (Latest.Element (I).Key_Str) = KStr then
                  Latest.Replace_Element (I, (To_Unbounded_String (KStr), Idx));
                  return;
               end if;
            end loop;
            Latest.Append ((To_Unbounded_String (KStr), Idx));
         end Set_Latest;
         function Get_Latest (KStr : String) return Natural is
         begin
            for U of Latest loop
               if To_String (U.Key_Str) = KStr then
                  return U.Idx;
               end if;
            end loop;
            return 0;
         end Get_Latest;
         function Subunit_Has_Generic_Ancestor (Name_Node : TREE) return Boolean is
            N : TREE := Name_Node;
         begin
            while not Is_Void_Tree (N) loop
               declare
                  Def : TREE := CONST_VOID;
               begin
                  if KIND (N) = DN_SELECTED then
                     Def := Canonical_Def (Get_Name_Defn (D (AS_DESIGNATOR, N)));
                     if not Is_Void_Tree (Def) and then KIND (Def) = DN_GENERIC_ID then
                        return True;
                     end if;
                     N := D (AS_NAME, N);
                  else
                     Def := Canonical_Def (Get_Name_Defn (N));
                     if not Is_Void_Tree (Def) and then KIND (Def) = DN_GENERIC_ID then
                        return True;
                     end if;
                     exit;
                  end if;
               end;
            end loop;
            return False;
         end Subunit_Has_Generic_Ancestor;

         procedure Scan_Nested_Generics (Item_S : TREE) is
            IL : SEQ_TYPE;
            It : TREE;
         begin
            if Is_Void_Tree (Item_S) then
               return;
            end if;
            IL := LIST (Item_S);
            while not IS_EMPTY (IL) loop
               POP (IL, It);
               if KIND (It) = DN_GENERIC_DECL then
                  declare
                     GID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
                     Hdr : constant TREE := D (AS_HEADER, It);
                  begin
                     if not Is_Void_Tree (GID) then
                        Ctx.Generic_Decls.Include (Tree_Key (GID), It);
                     end if;
                     if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                        Scan_Nested_Generics (D (AS_DECL_S1, Hdr));
                        Scan_Nested_Generics (D (AS_DECL_S2, Hdr));
                     end if;
                  end;
               elsif KIND (It) = DN_PACKAGE_DECL then
                  declare
                     Hdr : constant TREE := D (AS_HEADER, It);
                  begin
                     if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                        Scan_Nested_Generics (D (AS_DECL_S1, Hdr));
                        Scan_Nested_Generics (D (AS_DECL_S2, Hdr));
                     end if;
                  end;
               elsif KIND (It) = DN_PACKAGE_BODY then
                  declare
                     NPID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
                     NBdy : constant TREE := D (AS_BODY, It);
                  begin
                     if not Is_Void_Tree (NPID) and then KIND (NPID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (NPID))
                          or else Is_Void_Tree (NBdy)
                          or else KIND (NBdy) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (NPID), It);
                        end if;
                     end if;
                     if not Is_Void_Tree (NBdy) and then KIND (NBdy) = DN_BLOCK_BODY then
                        Scan_Nested_Generics (D (AS_ITEM_S, NBdy));
                     end if;
                  end;
               elsif KIND (It) = DN_SUBPROGRAM_BODY then
                  declare
                     NSID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
                     NBdy : constant TREE := D (AS_BODY, It);
                  begin
                     if not Is_Void_Tree (NSID) and then KIND (NSID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (NSID))
                          or else Is_Void_Tree (NBdy)
                          or else KIND (NBdy) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (NSID), It);
                        end if;
                     end if;
                  end;
               end if;
            end loop;
         end Scan_Nested_Generics;
      begin
         while not IS_EMPTY (Scan_L) loop
            POP (Scan_L, SCU);
            CU_Idx := CU_Idx + 1;
            declare
               Raw_AD   : constant TREE := D (AS_ALL_DECL, SCU);
               Was_Subu : constant Boolean := (KIND (Raw_AD) = DN_SUBUNIT);
               AD       : constant TREE := (if Was_Subu then D (AS_SUBUNIT_BODY, Raw_AD) else Raw_AD);
            begin
               if Was_Subu and then Subunit_Has_Generic_Ancestor (D (AS_NAME, Raw_AD)) then
                  if KIND (AD) = DN_TASK_BODY then
                     declare
                        T_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                        T_Sp : TREE := CONST_VOID;
                     begin
                        if not Is_Void_Tree (T_Id) then
                           if KIND (T_Id) = DN_TASK_BODY_ID then T_Sp := Resolve_Type_Spec (D (SM_TYPE_SPEC, T_Id));
                           elsif KIND (T_Id) = DN_VARIABLE_ID then T_Sp := Resolve_Type_Spec (D (SM_OBJ_TYPE, T_Id));
                           elsif KIND (T_Id) in CLASS_TYPE_NAME then T_Sp := Resolve_Type_Spec (D (SM_TYPE_SPEC, T_Id));
                           end if;
                           if not Is_Void_Tree (T_Sp) then
                              Ctx.Gen_Subunits.Include (Tree_Key (T_Sp), AD);
                           end if;
                        end if;
                     end;
                  elsif KIND (AD) in DN_SUBPROGRAM_BODY | DN_PACKAGE_BODY then
                     declare
                        S_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     begin
                        if not Is_Void_Tree (S_Id) then
                           Ctx.Gen_Subunits.Include (Tree_Key (S_Id), AD);
                        end if;
                     end;
                  end if;
               end if;
               if KIND (AD) = DN_PACKAGE_DECL and then not Was_Subu then
                  Set_Latest ("PS:" & Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, AD)), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                  if not Is_Void_Tree (D (AS_HEADER, AD)) and then KIND (D (AS_HEADER, AD)) = DN_PACKAGE_SPEC then
                     Scan_Nested_Generics (D (AS_DECL_S1, D (AS_HEADER, AD)));
                     Scan_Nested_Generics (D (AS_DECL_S2, D (AS_HEADER, AD)));
                  end if;
               elsif KIND (AD) = DN_PACKAGE_BODY then
                  declare
                     PID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     PBdy : constant TREE := D (AS_BODY, AD);
                  begin
                     if KIND (PID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (PID))
                          or else Is_Void_Tree (PBdy)
                          or else KIND (PBdy) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (PID), AD);
                        end if;
                        if not Is_Void_Tree (PBdy) and then KIND (PBdy) = DN_BLOCK_BODY then
                           Scan_Nested_Generics (D (AS_ITEM_S, PBdy));
                        end if;
                        if not Was_Subu then
                           declare
                              UPN : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           begin
                              Set_Latest ("PB:" & UPN, CU_Idx);
                              Append (In_Unit_Bodies, UPN & "|");
                           end;
                        end if;
                     else
                        if not Is_Void_Tree (PBdy) and then KIND (PBdy) = DN_BLOCK_BODY then
                           Scan_Nested_Generics (D (AS_ITEM_S, PBdy));
                        end if;
                        if not Was_Subu then
                           declare
                              UPN : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           begin
                              Set_Latest ("PB:" & UPN, CU_Idx);
                              Append (In_Unit_Bodies, UPN & "|");
                           end;
                        end if;
                     end if;
                  end;
               elsif KIND (AD) = DN_GENERIC_DECL then
                  declare
                     GID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     Hdr : constant TREE := D (AS_HEADER, AD);
                  begin
                     Ctx.Generic_Decls.Include (Tree_Key (GID), AD);
                     if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                        Scan_Nested_Generics (D (AS_DECL_S1, Hdr));
                        Scan_Nested_Generics (D (AS_DECL_S2, Hdr));
                     end if;
                     if not Was_Subu then
                        Set_Latest ("PS:" & Ada.Strings.Fixed.Translate (Sym_Name (GID), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                     end if;
                  end;
               elsif KIND (AD) = DN_SUBPROGRAM_BODY then
                  declare
                     SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     Hdr : constant TREE := D (AS_HEADER, AD);
                  begin
                     if KIND (SID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (SID))
                          or else Is_Void_Tree (D (AS_BODY, AD))
                          or else KIND (D (AS_BODY, AD)) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (SID), AD);
                        end if;
                        if not Was_Subu then
                           declare
                              UPN : constant String := Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           begin
                              Set_Latest ("PB:" & UPN, CU_Idx);
                              Append (In_Unit_Bodies, UPN & "|");
                           end;
                        end if;
                     elsif not Was_Subu then
                        Set_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                        if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PROCEDURE_SPEC
                          and then (Is_Void_Tree (D (AS_PARAM_S, Hdr)) or else IS_EMPTY (LIST (D (AS_PARAM_S, Hdr))))
                        then
                           Main_CU_Idx := CU_Idx;
                        end if;
                     end if;
                  end;
               elsif KIND (AD) = DN_SUBPROG_ENTRY_DECL and then not Was_Subu
                 and then not Is_Void_Tree (D (AS_UNIT_KIND, AD))
                 and then KIND (D (AS_UNIT_KIND, AD)) = DN_INSTANTIATION
               then
                  declare
                     SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     Sp  : constant TREE := (if not Is_Void_Tree (SID) then D (SM_SPEC, SID) else CONST_VOID);
                  begin
                     Set_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                     if not Is_Void_Tree (Sp) and then KIND (Sp) = DN_PROCEDURE_SPEC
                       and then (Is_Void_Tree (D (AS_PARAM_S, Sp)) or else IS_EMPTY (LIST (D (AS_PARAM_S, Sp))))
                     then
                        Main_CU_Idx := CU_Idx;
                     end if;
                  end;
               end if;
            end;
         end loop;

      CU_Idx := 0;
      while not IS_EMPTY (CU_List) loop
         POP (CU_List, CU);
         CU_Idx := CU_Idx + 1;
         if Only_CU = 0 or else CU_Idx = Only_CU then
         declare
            Raw_All  : constant TREE := D (AS_ALL_DECL, CU);
            All_Decl : TREE := Raw_All;
            K        : NODE_NAME := KIND (All_Decl);
            Is_Subu  : constant Boolean := (K = DN_SUBUNIT);
            function Subunit_Parent_Prefix (N : TREE) return String is
            begin
               if Is_Void_Tree (N) then return ""; end if;
               if KIND (N) = DN_SELECTED then
                  return Subunit_Parent_Prefix (D (AS_NAME, N)) & "__" & Sanitize_Ident (Sym_Name (D (AS_DESIGNATOR, N)));
               else
                  return Sanitize_Ident (Sym_Name (N));
               end if;
            end Subunit_Parent_Prefix;
         begin
            Ctx.In_Subunit := Is_Subu;
            if K = DN_SUBUNIT then
               if Subunit_Has_Generic_Ancestor (D (AS_NAME, Raw_All)) then
                  All_Decl := CONST_VOID;
                  K := DN_VOID;
               else
                  All_Decl := D (AS_SUBUNIT_BODY, All_Decl);
                  K := KIND (All_Decl);
               end if;
            end if;
            if K = DN_TASK_BODY then
               Emit_Task_Body (Ctx, All_Decl);
            elsif K = DN_GENERIC_DECL then
               declare
                  GID   : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                  UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (GID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  PName : constant String := Lower_Name (Sym_Name (GID));
                  GSym  : constant String := "@ada_gelab__" & Qualified_Ada_Name (GID);
                  Saved : Pir_Emit.Func_Buffer;
               begin
                  if not Is_Subu and then Get_Latest ("PS:" & UPN) = CU_Idx then
                     Add_Global_Decl (Ctx.E, GSym, "export " & GSym & " = global i8 = 0, align 1");
                     Ctx.Gen_Elab_Flags.Include (Tree_Key (GID), To_Unbounded_String (GSym));
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_spec() -> void cc ""internal""");
                     declare
                        GA : constant String := Next_Reg (Ctx.E, "lgea");
                     begin
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        Emit_Inst (Ctx.E, "store.i8 0, " & GA & ", align 1");
                     end;
                     Emit_Term (Ctx.E, "ret");
                     End_Func (Ctx.E, Saved);
                  end if;
               end;

            elsif K = DN_SUBPROGRAM_BODY then
               declare
                  SID        : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                  Is_Subunit : constant Boolean := Is_Subu;
               begin
                  if KIND (SID) = DN_GENERIC_ID then
                     declare
                        UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                        PName : constant String := Lower_Name (Sym_Name (SID));
                        GSym  : constant String := "@ada_gelab__" & Qualified_Ada_Name (SID);
                        Saved : Pir_Emit.Func_Buffer;
                     begin
                        if not Is_Subunit and then Get_Latest ("PB:" & UPN) = CU_Idx then
                           if not Ctx.Gen_Elab_Flags.Contains (Tree_Key (SID)) then
                              Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global i8, align 1");
                           end if;
                           Begin_Func (Ctx.E, Saved,
                             "export func @ada__" & PName & "__elab_body() -> void cc ""internal""");
                           declare
                              GA : constant String := Next_Reg (Ctx.E, "lge1");
                           begin
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                           end;
                           Emit_Term (Ctx.E, "ret");
                           End_Func (Ctx.E, Saved);
                        end if;
                     end;
                  else
                     if Is_Subunit or else Get_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map)) = CU_Idx then
                        Emit_Subprogram_Body (Ctx, All_Decl, Is_Exported => True, Is_Subunit => Is_Subunit);
                        if Ctx.Emit_Main and then not Is_Subunit and then CU_Idx = Main_CU_Idx then
                           declare
                              SI : constant Subp_Info := Ctx.Subps.Element (Tree_Key (SID));
                           begin
                              Emit_Main_Wrapper (Ctx, To_String (SI.Pir_Name), Comp_Node, To_String (In_Unit_Bodies));
                           end;
                        end if;
                     end if;
                  end if;
               end;

            elsif K = DN_SUBPROG_ENTRY_DECL then
               declare
                  SID   : constant TREE := D (AS_SOURCE_NAME, All_Decl);
                  PName : constant String := Ada_Sym_Util.Sanitize_Ada_Ident (Sym_Name (SID));
                  UK    : constant TREE := D (AS_UNIT_KIND, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION
                    and then (Is_Subu or else Get_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map)) = CU_Idx)
                  then
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_spec() -> void cc ""internal""");
                     Emit_Generic_Instantiation (Ctx, All_Decl, Export_All => True);
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "ret");
                     end if;
                     End_Func (Ctx.E, Saved);
                     if Ctx.Emit_Main and then not Is_Subu and then CU_Idx = Main_CU_Idx then
                        Emit_Main_Wrapper (Ctx, Ada_Sym_Util.Main_Entry_Sym (Sym_Name (SID)), Comp_Node, To_String (In_Unit_Bodies));
                     end if;
                  end if;
               end;

            elsif K = DN_PACKAGE_DECL then
               declare
                  PID   : constant TREE := D (AS_SOURCE_NAME, All_Decl);
                  UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  PName : constant String := Lower_Name (Sym_Name (PID));
                  UK    : constant TREE := D (AS_UNIT_KIND, All_Decl);
                  Hdr   : constant TREE := D (AS_HEADER, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
                  Dyn1, Dyn2 : Boolean;
               begin
                  if Is_Subu or else Get_Latest ("PS:" & UPN) = CU_Idx then
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_spec() -> void cc ""internal""");
                     if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                        Emit_Generic_Instantiation (Ctx, All_Decl, Export_All => True);
                     elsif not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                        declare
                           Has_Body   : constant Boolean := Package_Has_Compiled_Body (UPN, To_String (In_Unit_Bodies), To_String (Ctx.Lib_Dir));
                           Spec_Has_T : constant Boolean :=
                             Item_List_Has_Tasks (Ctx, D (AS_DECL_S1, Hdr))
                             or else Item_List_Has_Tasks (Ctx, D (AS_DECL_S2, Hdr));
                           Saved_G    : constant Integer_64 := Ctx.Cur_Task_Group_Id;
                           Pkg_Gid    : constant Integer_64 := Integer_64 (Lib_Binder.Lib_Pkg_Task_Group_Id (UPN));
                           GM_R, Dummy : Unbounded_String;
                        begin
                           if Spec_Has_T then
                              Ctx.Cur_Task_Group_Id := Pkg_Gid;
                           end if;
                           Emit_Decl_Items (Ctx, D (AS_DECL_S1, Hdr), Is_Global => True, Export_All => True, Has_Dyn => Dyn1);
                           Emit_Decl_Items (Ctx, D (AS_DECL_S2, Hdr), Is_Global => True, Export_All => True, Has_Dyn => Dyn2);
                           if Spec_Has_T then
                              Ctx.Cur_Task_Group_Id := Saved_G;
                              -- In single-file mode without external binder, activate bodyless spec tasks right away
                              if Ctx.Emit_Main and then not Has_Body then
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM_R, Nounwind => True);
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_group",
                                   To_String (GM_R) & ", " & Img (Long_Long_Integer (Pkg_Gid)), "", Dummy);
                              end if;
                           end if;
                        end;
                     end if;
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "ret");
                     end if;
                     End_Func (Ctx.E, Saved);
                  end if;
               end;

            elsif K = DN_PACKAGE_BODY then
               declare
                  PID   : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                  UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  Sub_P : constant String := (if Is_Subu then Subunit_Parent_Prefix (D (AS_NAME, Raw_All)) else "");
                  Q_PN  : constant String := Qualified_Ada_Name (PID);
                  PName : constant String :=
                    (if not Is_Subu then Lower_Name (Sym_Name (PID))
                     elsif Ada.Strings.Fixed.Index (Q_PN, "__") > 0 then Q_PN
                     elsif Sub_P'Length > 0 then Sub_P & "__" & Sanitize_Ident (Sym_Name (PID))
                     else Q_PN);
                  Bdy   : constant TREE := D (AS_BODY, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
                  Dyn   : Boolean;
                  Is_Sup : constant Boolean :=
                    UPN = "REPORT" or else UPN = "FCNDECL" or else UPN = "SPPRT13" or else UPN = "CHECKFILE";
               begin
                  if KIND (PID) = DN_GENERIC_ID then
                     if not Is_Subu and then Get_Latest ("PB:" & UPN) = CU_Idx then
                        declare
                           GSym : constant String := "@ada_gelab__" & Qualified_Ada_Name (PID);
                        begin
                           if not Ctx.Gen_Elab_Flags.Contains (Tree_Key (PID)) then
                              Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global i8, align 1");
                           end if;
                           Begin_Func (Ctx.E, Saved,
                             "export func @ada__" & PName & "__elab_body() -> void cc ""internal""");
                           declare
                              GA : constant String := Next_Reg (Ctx.E, "lge1");
                           begin
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                           end;
                           Emit_Term (Ctx.E, "ret");
                           End_Func (Ctx.E, Saved);
                        end;
                     end if;
                  elsif Is_Subu or else Get_Latest ("PB:" & UPN) = CU_Idx then
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_body() -> void cc ""internal""");
                     if not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_BLOCK_BODY then
                        declare
                           Spec_Hdr   : constant TREE :=
                             (if KIND (PID) = DN_PACKAGE_ID then D (SM_SPEC, PID) else CONST_VOID);
                           Spec_Has_T : constant Boolean :=
                             not Is_Void_Tree (Spec_Hdr) and then KIND (Spec_Hdr) = DN_PACKAGE_SPEC
                             and then (Item_List_Has_Tasks (Ctx, D (AS_DECL_S1, Spec_Hdr))
                                       or else Item_List_Has_Tasks (Ctx, D (AS_DECL_S2, Spec_Hdr)));
                           Bdy_Has_T  : constant Boolean := Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Bdy));
                           Any_Has_T  : constant Boolean := Spec_Has_T or else Bdy_Has_T;
                           Saved_G    : constant Integer_64 := Ctx.Cur_Task_Group_Id;
                           Gid_Name   : constant String :=
                             (if Is_Subu then Ada.Strings.Fixed.Translate (PName, Ada.Strings.Maps.Constants.Upper_Case_Map)
                              else UPN);
                           Pkg_Gid    : constant Integer_64 := Integer_64 (Lib_Binder.Lib_Pkg_Task_Group_Id (Gid_Name));
                           GM_R, Dummy : Unbounded_String;
                           Saved_Mst   : constant Unbounded_String := Ctx.Cur_Master_Reg;
                           Subu_Mst    : Unbounded_String;
                           Prev_LPB : constant Boolean := Ctx.In_Lib_Pkg_Body;
                           Alt_S    : constant TREE := D (AS_ALTERNATIVE_S, Bdy);
                           Has_Hdl  : constant Boolean := not Is_Void_Tree (Alt_S) and then not IS_EMPTY (LIST (Alt_S));
                           Hdl_Lbl, After_Lbl : Unbounded_String;
                        begin
                           -- P3-4.9: Separate local package body subunit attaches & activates tasks on enclosing active master!
                           if Is_Subu then
                              declare
                                 AM_R  : Unbounded_String;
                                 GM_F  : Unbounded_String;
                                 Is_N  : constant String := Next_Reg (Ctx.E, "samn");
                                 Sel_M : constant String := Next_Reg (Ctx.E, "samm");
                              begin
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_get_active_master", "", "ptr", AM_R, Nounwind => True);
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM_F, Nounwind => True);
                                 Emit_Inst (Ctx.E, Is_N & " = cmp_eq.ptr " & To_String (AM_R) & ", 0");
                                 Emit_Inst (Ctx.E, Sel_M & " = select.ptr " & Is_N & ", " & To_String (GM_F) & ", " & To_String (AM_R));
                                 Subu_Mst := To_Unbounded_String (Sel_M);
                                 Ctx.Cur_Master_Reg := Subu_Mst;
                              end;
                           end if;
                           if Any_Has_T then
                              Ctx.Cur_Task_Group_Id := Pkg_Gid;
                           end if;
                           Ctx.In_Lib_Pkg_Body := (not Is_Subu and then not Is_Sup);
                           Emit_Decl_Items (Ctx, D (AS_ITEM_S, Bdy), Is_Global => True, Export_All => True, Has_Dyn => Dyn);
                           Ctx.In_Lib_Pkg_Body := Prev_LPB;
                           if Any_Has_T then
                              Ctx.Cur_Task_Group_Id := Saved_G;
                           end if;
                           if Has_Hdl then
                              Hdl_Lbl   := To_Unbounded_String (Next_Label (Ctx.E, "lpexc_h"));
                              After_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "lpexc_end"));
                              Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                                    Current_Exn   => Null_Unbounded_String,
                                                    In_Handler    => False,
                                                    SS_Mark       => Null_Unbounded_String,
                                                    SP_Mark       => Null_Unbounded_String));
                           end if;
                           if Any_Has_T then
                              if Is_Subu and then Length (Subu_Mst) > 0 then
                                 GM_R := Subu_Mst;
                              else
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM_R, Nounwind => True);
                              end if;
                              Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_group",
                                To_String (GM_R) & ", " & Img (Long_Long_Integer (Pkg_Gid)), "", Dummy);
                           end if;
                           Scan_Label_Cleanup_Depths (Ctx, D (AS_STM_S, Bdy), Cleanup_Depth (Ctx));
                           Scan_Label_Cleanup_Depths (Ctx, Alt_S, Cleanup_Depth (Ctx));
                           Emit_Stm_List (Ctx, D (AS_STM_S, Bdy));
                           if Has_Hdl then
                              if not Is_Terminated (Ctx.E) then
                                 Emit_Term (Ctx.E, "jmp " & To_String (After_Lbl));
                              end if;
                              Emit_Exception_Handlers (Ctx, Alt_S, To_String (Hdl_Lbl), To_String (After_Lbl));
                              Ctx.Handlers.Delete_Last;
                              Emit_Label (Ctx.E, To_String (After_Lbl));
                           end if;
                           Ctx.Cur_Master_Reg := Saved_Mst;
                        end;
                     end if;
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "ret");
                     end if;
                     End_Func (Ctx.E, Saved);
                  end if;
               end;
            end if;
         end;
         end if;
      end loop;
      Ctx.In_Subunit := False;
      end;
   end Emit_Compilation;

end Cg_Stmt.Subps;
