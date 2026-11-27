-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr; use Cg_Expr;
with Cg_Stmt.Decls;
with Cg_Stmt.Generics;
with Cg_Stmt.Subps;

package body Cg_Stmt is

   -- Forwarding wrappers to domain child packages (Cg_Stmt.Generics, Cg_Stmt.Decls, Cg_Stmt.Subps)
   function Find_Gen_Subunit_Body (Ctx : Context; Item : TREE) return TREE is
   begin
      return Cg_Stmt.Generics.Find_Gen_Subunit_Body (Ctx, Item);
   end Find_Gen_Subunit_Body;

   function Is_Gen_Subunit_Stub (Ctx : Context; Item : TREE) return Boolean is
   begin
      return Cg_Stmt.Generics.Is_Gen_Subunit_Stub (Ctx, Item);
   end Is_Gen_Subunit_Stub;

   function Is_RTL_Generic (Gen_Id : TREE) return Boolean is
   begin
      return Cg_Stmt.Generics.Is_RTL_Generic (Gen_Id);
   end Is_RTL_Generic;

   function Tree_Uses_SS (Ctx : in out Context; Node : TREE) return Boolean is
   begin
      return Cg_Stmt.Generics.Tree_Uses_SS (Ctx, Node);
   end Tree_Uses_SS;

   function Tree_Has_Return (Node : TREE) return Boolean is
   begin
      return Cg_Stmt.Generics.Tree_Has_Return (Node);
   end Tree_Has_Return;

   procedure Map_Decl_List_Ids (Ctx : in out Context; Gen_DS : TREE; Inst_DS : TREE) is
   begin
      Cg_Stmt.Generics.Map_Decl_List_Ids (Ctx, Gen_DS, Inst_DS);
   end Map_Decl_List_Ids;

   procedure Scan_Tree_For_Captures
     (Ctx           : in out Context;
      Node          : TREE;
      Current_Subp  : Unsigned_32;
      Owner_Map     : Var_Maps.Map;
      Parent_Map    : in out Subp_Maps.Map;
      Captured_Vars : in out Cap_Vecs.Vector;
      Nested_Subps  : in out U32_Vecs.Vector;
      Target_Owner  : Unsigned_32;
      Type_Depth    : Natural := 0;
      Inst_Key      : Unsigned_32 := 0;
      Inst_Gen_Id   : TREE := CONST_VOID)
   is
   begin
      Cg_Stmt.Generics.Scan_Tree_For_Captures
        (Ctx, Node, Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps,
         Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
   end Scan_Tree_For_Captures;

   procedure Emit_Generic_Instantiation
     (Ctx        : in out Context;
      Inst_Decl  : TREE;
      Export_All : Boolean)
   is
   begin
      Cg_Stmt.Generics.Emit_Generic_Instantiation (Ctx, Inst_Decl, Export_All);
   end Emit_Generic_Instantiation;

   function Is_Named_No_Others_Agg (AN : TREE; Rem_D : Natural) return Boolean is
   begin
      return Cg_Stmt.Decls.Is_Named_No_Others_Agg (AN, Rem_D);
   end Is_Named_No_Others_Agg;

   procedure Pre_Register_Subp
     (Ctx         : in out Context;
      Subp_Id     : TREE;
      Spec_Node   : TREE;
      Parent_Subp : Unsigned_32;
      Is_Subunit  : Boolean := False)
   is
   begin
      Cg_Stmt.Decls.Pre_Register_Subp (Ctx, Subp_Id, Spec_Node, Parent_Subp, Is_Subunit);
   end Pre_Register_Subp;

   procedure Emit_Decl_Items
     (Ctx        : in out Context;
      Item_S     : TREE;
      Is_Global  : Boolean;
      Export_All : Boolean;
      Has_Dyn    : out Boolean;
      Only_Item  : TREE := CONST_VOID)
   is
   begin
      Cg_Stmt.Decls.Emit_Decl_Items (Ctx, Item_S, Is_Global, Export_All, Has_Dyn, Only_Item);
   end Emit_Decl_Items;

   procedure Emit_Subprogram_Body
     (Ctx           : in out Context;
      Body_Node     : TREE;
      Override_Id   : TREE := CONST_VOID;
      Override_Spec : TREE := CONST_VOID;
      Is_Exported   : Boolean := False;
      Is_Subunit    : Boolean := False)
   is
   begin
      Cg_Stmt.Subps.Emit_Subprogram_Body (Ctx, Body_Node, Override_Id, Override_Spec, Is_Exported, Is_Subunit);
   end Emit_Subprogram_Body;

   procedure Emit_Task_Body
     (Ctx       : in out Context;
      Body_Node : TREE)
   is
   begin
      Cg_Stmt.Subps.Emit_Task_Body (Ctx, Body_Node);
   end Emit_Task_Body;

   procedure Emit_Compilation (Ctx : in out Context; Comp_Node : TREE; Only_CU : Natural := 0) is
   begin
      Cg_Stmt.Subps.Emit_Compilation (Ctx, Comp_Node, Only_CU);
   end Emit_Compilation;

   -- =========================================================================
   -- Statements
   -- =========================================================================

   procedure Emit_Stm_List (Ctx : in out Context; Stm_S : TREE) is
      L : SEQ_TYPE;
      S : TREE;
   begin
      if Is_Void_Tree (Stm_S) then
         return;
      end if;
      L := LIST (Stm_S);
      while not IS_EMPTY (L) loop
         POP (L, S);
         if not Is_Terminated (Ctx.E) or else KIND (S) = DN_LABELED then
            Emit_Stm (Ctx, S);
         end if;
      end loop;
   end Emit_Stm_List;

   procedure Emit_Exception_Handlers
     (Ctx        : in out Context;
      Alt_S      : TREE;
      Hdl_Label  : String;
      After_Lbl  : String)
   is
      Exn_Reg : constant String := Next_Reg (Ctx.E, "exn");
      Alts    : SEQ_TYPE := LIST (Alt_S);
      Alt     : TREE;
      Has_Others : Boolean := False;
      Top_Idx : constant Natural := Natural (Ctx.Handlers.Length) - 1;
   begin
      -- Mark handler entry as In_Handler so calls inside handler propagate to outer handler (not self!)
      declare
         HE : Handler_Entry := Ctx.Handlers.Element (Top_Idx);
      begin
         HE.In_Handler  := True;
         HE.Current_Exn := To_Unbounded_String (Exn_Reg);
         Ctx.Handlers.Replace_Element (Top_Idx, HE);
      end;

      Emit_Label (Ctx.E, Hdl_Label, Exn_Reg & ": ptr");

      -- LRM 9.10(6): beginning of an exception handler is a mandatory abort sync point
      declare
         Dummy_Ab : Unbounded_String;
      begin
         Emit_Call_Or_Invoke (Ctx, "@__ada_check_abort", "", "", Dummy_Ab, Nounwind => True);
      end;

      -- Reclaim any Secondary Stack temporaries & inner alloca_dyn allocations from the failed try body!
      declare
         HE : constant Handler_Entry := Ctx.Handlers.Element (Top_Idx);
         Dummy : Unbounded_String;
      begin
         if Length (HE.SS_Mark) > 0 then
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (HE.SS_Mark), "", Dummy, Nounwind => True);
         end if;
         if Length (HE.SP_Mark) > 0 then
            Emit_Inst (Ctx.E, "stackrestore " & To_String (HE.SP_Mark));
         end if;
      end;

      while not IS_EMPTY (Alts) loop
         POP (Alts, Alt);
         if KIND (Alt) = DN_ALTERNATIVE then
            declare
               CS       : SEQ_TYPE := LIST (D (AS_CHOICE_S, Alt));
               Ch       : TREE;
               Is_Oth   : Boolean := False;
               Cond_Reg : Unbounded_String;
            begin
               while not IS_EMPTY (CS) loop
                  POP (CS, Ch);
                  if KIND (Ch) = DN_CHOICE_OTHERS then
                     Is_Oth := True;
                     Has_Others := True;
                  elsif KIND (Ch) = DN_CHOICE_EXP then
                     declare
                        EN : TREE := D (AS_EXP, Ch);
                        ED : TREE;
                     begin
                        if KIND (EN) = DN_SELECTED then
                           EN := D (AS_DESIGNATOR, EN);
                        end if;
                        ED := D (SM_DEFN, EN);
                        declare
                           Tag : constant String := Get_Exc_Tag (Ctx, ED);
                           TA  : constant String := Next_Reg (Ctx.E, "et");
                           Eq  : constant String := Next_Reg (Ctx.E, "eq");
                        begin
                           Emit_Inst (Ctx.E, TA & " = addr " & Tag);
                           Emit_Inst (Ctx.E, Eq & " = cmp_eq.ptr " & Exn_Reg & ", " & TA);
                           if Length (Cond_Reg) = 0 then
                              Cond_Reg := To_Unbounded_String (Eq);
                           else
                              declare
                                 OrR : constant String := Next_Reg (Ctx.E, "eor");
                              begin
                                 Emit_Inst (Ctx.E, OrR & " = or.i8 " & To_String (Cond_Reg) & ", " & Eq);
                                 Cond_Reg := To_Unbounded_String (OrR);
                              end;
                           end if;
                        end;
                     end;
                  end if;
               end loop;

               if Is_Oth then
                  Emit_Stm_List (Ctx, D (AS_STM_S, Alt));
                  if not Is_Terminated (Ctx.E) then
                     Emit_Term (Ctx.E, "jmp " & After_Lbl);
                  end if;
               elsif Length (Cond_Reg) > 0 then
                  declare
                     L_Match : constant String := Next_Label (Ctx.E, "eh_m");
                     L_Next  : constant String := Next_Label (Ctx.E, "eh_n");
                  begin
                     Emit_Term (Ctx.E, "br " & To_String (Cond_Reg) & ", " & L_Match & ", " & L_Next);
                     Emit_Label (Ctx.E, L_Match);
                     Emit_Stm_List (Ctx, D (AS_STM_S, Alt));
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "jmp " & After_Lbl);
                     end if;
                     Emit_Label (Ctx.E, L_Next);
                  end;
               end if;
            end;
         end if;
      end loop;

      -- Unmatched exception: re-propagate to outer handler or caller
      if not Has_Others and then not Is_Terminated (Ctx.E) then
         Emit_Raise_Reg (Ctx, Exn_Reg);
      end if;
   end Emit_Exception_Handlers;

   function Type_Contains_Task (Ctx : in out Context; Raw_T : TREE; Depth : Natural := 0) return Boolean is
      R : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Raw_T));
   begin
      if Is_Void_Tree (R) or else Depth > 8 then
         return False;
      end if;
      if Is_Task_Type (R) then
         return True;
      elsif Is_Constrained_Array (R) or else KIND (R) = DN_ARRAY then
         declare
            BT : constant TREE := Subst_Base_Type (Ctx, R);
         begin
            return not Is_Void_Tree (BT) and then KIND (BT) = DN_ARRAY
              and then Type_Contains_Task (Ctx, D (SM_COMP_TYPE, BT), Depth + 1);
         end;
      elsif Is_Record_Type (R) then
         declare
            BT : constant TREE := Subst_Base_Type (Ctx, R);

            function Comp_List_Contains_Task (CL : TREE; Dp : Natural) return Boolean is
            begin
               if Is_Void_Tree (CL) or else Dp > 8 then
                  return False;
               end if;
               if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                  declare
                     DL   : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                     Decl : TREE;
                  begin
                     while not IS_EMPTY (DL) loop
                        POP (DL, Decl);
                        if KIND (Decl) = DN_VARIABLE_DECL then
                           declare
                              CID : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, Decl)));
                           begin
                              if Type_Contains_Task (Ctx, D (SM_OBJ_TYPE, CID), Dp + 1) then
                                 return True;
                              end if;
                           end;
                        end if;
                     end loop;
                  end;
               end if;
               declare
                  VP : constant TREE := D (AS_VARIANT_PART, CL);
               begin
                  if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART
                    and then not Is_Void_Tree (D (AS_VARIANT_S, VP))
                  then
                     declare
                        VL : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                        VN : TREE;
                     begin
                        while not IS_EMPTY (VL) loop
                           POP (VL, VN);
                           if KIND (VN) = DN_VARIANT
                             and then Comp_List_Contains_Task (D (AS_COMP_LIST, VN), Dp + 1)
                           then
                              return True;
                           end if;
                        end loop;
                     end;
                  end if;
               end;
               return False;
            end Comp_List_Contains_Task;
         begin
            if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
               if Comp_List_Contains_Task (D (SM_COMP_LIST, BT), Depth) then
                  return True;
               end if;
            end if;
         end;
      end if;
      return False;
   end Type_Contains_Task;

   function Item_List_Has_Tasks (Ctx : in out Context; Item_S : TREE) return Boolean is
      L  : SEQ_TYPE;
      It : TREE;
   begin
      if Is_Void_Tree (Item_S) then
         return False;
      end if;
      L := LIST (Item_S);
      while not IS_EMPTY (L) loop
         POP (L, It);
         case KIND (It) is
            when DN_TASK_DECL | DN_TASK_BODY =>
               return True;
            when DN_TYPE_DECL =>
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, It);
                  TS  : constant TREE :=
                    (if not Is_Void_Tree (TID) and then KIND (TID) in CLASS_TYPE_NAME
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TID))) else CONST_VOID);
               begin
                  if not Is_Void_Tree (TS) and then KIND (TS) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                    and then Type_Contains_Task (Ctx, D (SM_DESIG_TYPE, TS))
                  then
                     return True;
                  end if;
               end;
            when DN_VARIABLE_DECL =>
               declare
                  VID : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, It)));
               begin
                  if Type_Contains_Task (Ctx, D (SM_OBJ_TYPE, VID)) then
                     return True;
                  end if;
               end;
            when DN_PACKAGE_DECL =>
               declare
                  UK  : constant TREE := D (AS_UNIT_KIND, It);
                  Hdr : constant TREE :=
                    (if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION
                     then D (SM_SPEC, Canonical_Def (D (AS_SOURCE_NAME, It)))
                     else D (AS_HEADER, It));
               begin
                  if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                     if Item_List_Has_Tasks (Ctx, D (AS_DECL_S1, Hdr))
                       or else Item_List_Has_Tasks (Ctx, D (AS_DECL_S2, Hdr))
                     then
                        return True;
                     end if;
                  end if;
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     declare
                        Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                        GK     : constant Unsigned_32 := Tree_Key (Gen_Id);
                        GB     : TREE := CONST_VOID;
                     begin
                        if not Is_Void_Tree (Gen_Id) then
                           if Ctx.Generic_Body.Contains (GK) then
                              GB := Ctx.Generic_Body.Element (GK);
                           elsif KIND (Gen_Id) = DN_GENERIC_ID then
                              GB := D (SM_BODY, Gen_Id);
                              if Is_Void_Tree (GB) then
                                 GB := D (XD_BODY, Gen_Id);
                              end if;
                           end if;
                        end if;
                        if not Is_Void_Tree (GB) then
                           declare
                              BB : constant TREE := (if KIND (GB) = DN_PACKAGE_BODY then D (AS_BODY, GB) else GB);
                              Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
                              GDecl : constant TREE :=
                                (if Ctx.Generic_Decls.Contains (GK) then Ctx.Generic_Decls.Element (GK)
                                 else CONST_VOID);
                              Param_S : constant TREE :=
                                (if not Is_Void_Tree (GDecl) and then KIND (GDecl) = DN_GENERIC_DECL then D (AS_ITEM_S, GDecl)
                                 elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_GENERIC_PARAM_S, Gen_Id)
                                 else CONST_VOID);
                              ADecls_N : constant TREE := D (SM_DECL_S, UK);
                              Res_Has : Boolean := False;
                           begin
                              -- Temporarily map formal types -> actual types so tasks via formal limited private types are detected!
                              if not Is_Void_Tree (Param_S) and then not Is_Void_Tree (ADecls_N) then
                                 declare
                                    FL : SEQ_TYPE := LIST (Param_S);
                                    AL : SEQ_TYPE := LIST (ADecls_N);
                                    FI, AD : TREE;
                                 begin
                                    while not IS_EMPTY (FL) and then not IS_EMPTY (AL) loop
                                       POP (FL, FI);
                                       if KIND (FI) in DN_IN | DN_IN_OUT then
                                          declare
                                             FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FI));
                                             Dummy_F : TREE;
                                          begin
                                             while not IS_EMPTY (FNS) and then not IS_EMPTY (AL) loop
                                                POP (FNS, Dummy_F);
                                                POP (AL, AD);
                                             end loop;
                                          end;
                                       else
                                          POP (AL, AD);
                                          if KIND (FI) = DN_TYPE_DECL and then KIND (AD) = DN_SUBTYPE_DECL then
                                             declare
                                                F_Tid : constant TREE := D (AS_SOURCE_NAME, FI);
                                                F_Ts  : constant TREE := D (SM_TYPE_SPEC, F_Tid);
                                                A_Sid : constant TREE := D (AS_SOURCE_NAME, AD);
                                                A_Ts  : constant TREE := D (SM_TYPE_SPEC, A_Sid);
                                             begin
                                                Ctx.Generic_Subst.Include (Tree_Key (F_Tid), A_Sid);
                                                if not Is_Void_Tree (F_Ts) and then not Is_Void_Tree (A_Ts) then
                                                   Ctx.Generic_Subst.Include (Tree_Key (F_Ts), A_Ts);
                                                end if;
                                             end;
                                          end if;
                                       end if;
                                    end loop;
                                 end;
                              end if;
                              if not Is_Void_Tree (BB) and then KIND (BB) = DN_BLOCK_BODY then
                                 Res_Has := Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, BB));
                              end if;
                              Ctx.Generic_Subst := Saved_Sub;
                              if Res_Has then
                                 return True;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;
            when DN_PACKAGE_BODY =>
               declare
                  Bdy : constant TREE := D (AS_BODY, It);
               begin
                  if not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_BLOCK_BODY then
                     if Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Bdy)) then
                        return True;
                     end if;
                  elsif not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_STUB then
                     -- P3-4.9: Separate local package body subunit may declare tasks attached to enclosing master
                     return True;
                  end if;
               end;
            when others =>
               null;
         end case;
      end loop;
      return False;
   end Item_List_Has_Tasks;

   procedure Scan_Label_Cleanup_Depths
     (Ctx   : in out Context;
      Node  : TREE;
      Depth : Natural)
   is
      K : NODE_NAME;
   begin
      if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then
         return;
      end if;
      K := KIND (Node);
      if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then
         return;
      end if;
      if K = DN_LABELED then
         declare
            LS  : constant TREE := D (AS_SOURCE_NAME_S, Node);
            LL  : SEQ_TYPE;
            LID : TREE;
         begin
            if not Is_Void_Tree (LS) then
               LL := LIST (LS);
               while not IS_EMPTY (LL) loop
                  POP (LL, LID);
                  Ctx.Label_Cleanup_Depths.Include (Tree_Key (LID), Integer_64 (Depth));
               end loop;
            end if;
         end;
         Scan_Label_Cleanup_Depths (Ctx, D (AS_STM, Node), Depth);
         return;
      elsif K = DN_BLOCK then
         declare
            BB    : constant TREE := D (AS_BLOCK_BODY, Node);
            NextD : Natural := Depth + 1; -- CK_SP_Mark pushed for every non-subp DN_BLOCK
         begin
            if not Is_Void_Tree (BB) then
               if Tree_Uses_SS (Ctx, BB) and then not (Ctx.Cur_Ret_Uncon and then Tree_Has_Return (BB)) then
                  NextD := NextD + 1;
               end if;
               if Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, BB)) then
                  NextD := NextD + 1;
               end if;
               Scan_Label_Cleanup_Depths (Ctx, D (AS_STM_S, BB), NextD);
               Scan_Label_Cleanup_Depths (Ctx, D (AS_ALTERNATIVE_S, BB), NextD);
            end if;
         end;
         return;
      elsif K = DN_LOOP then
         declare
            Iter    : constant TREE := D (AS_ITERATION, Node);
            BStms   : constant TREE := D (AS_STM_S, Node);
            Cond_SS : constant Boolean :=
              not Is_Void_Tree (Iter) and then KIND (Iter) = DN_WHILE and then Tree_Uses_SS (Ctx, D (AS_EXP, Iter));
            Loop_SS : constant Boolean := Tree_Uses_SS (Ctx, BStms) or else Cond_SS;
            NextD   : constant Natural := Depth + (if Loop_SS then 1 else 0);
         begin
            Scan_Label_Cleanup_Depths (Ctx, BStms, NextD);
         end;
         return;
      end if;
      if ARITY (Node) = ARBITRARY then
         declare
            L    : SEQ_TYPE := LIST (Node);
            Item : TREE;
         begin
            while not IS_EMPTY (L) loop
               POP (L, Item);
               Scan_Label_Cleanup_Depths (Ctx, Item, Depth);
            end loop;
         end;
      else
         for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
            Scan_Label_Cleanup_Depths (Ctx, DABS (I, Node), Depth);
         end loop;
      end if;
   end Scan_Label_Cleanup_Depths;

   procedure Emit_Block_Body
     (Ctx        : in out Context;
      Block_Node : TREE;
      Is_Subp    : Boolean)
   is
      Item_S    : constant TREE := D (AS_ITEM_S, Block_Node);
      Stm_S     : constant TREE := D (AS_STM_S, Block_Node);
      Alt_S     : constant TREE := D (AS_ALTERNATIVE_S, Block_Node);
      Has_Hdl   : constant Boolean := (not Is_Void_Tree (Alt_S)) and then (not IS_EMPTY (LIST (Alt_S)));
      Has_Tasks : constant Boolean := Item_List_Has_Tasks (Ctx, Item_S);
      Hdl_Lbl   : Unbounded_String;
      After_Lbl : Unbounded_String;
      Saved_SP  : Unbounded_String;
      Has_Dyn   : Boolean := False;
      Has_SS    : Boolean := False;
      Saved_SS  : Unbounded_String;
      Saved_Mst : constant Unbounded_String := Ctx.Cur_Master_Reg;
      Local_Mst : Unbounded_String;
   begin
      if not Is_Subp then
         declare
            SP : constant String := Next_Reg (Ctx.E, "sp");
         begin
            Emit_Inst (Ctx.E, SP & " = stacksave");
            Saved_SP := To_Unbounded_String (SP);
            Push_Cleanup (Ctx, CK_SP_Mark, SP);
         end;
         if Tree_Uses_SS (Ctx, Block_Node)
           and then not (Ctx.Cur_Ret_Uncon and then Tree_Has_Return (Block_Node))
         then
            Has_SS := True;
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", Saved_SS, Nounwind => True);
            Push_Cleanup (Ctx, CK_SS_Mark, To_String (Saved_SS));
         end if;
      end if;

      declare
         MDec_Exc : Unbounded_String;
         Mst_Exc  : Unbounded_String;
         Mst_Done : Unbounded_String;
      begin
         if Has_Tasks then
            Emit_Call_Or_Invoke (Ctx, "@__ada_master_enter", "", "ptr", Local_Mst);
            Ctx.Cur_Master_Reg := Local_Mst;
            Push_Cleanup (Ctx, CK_Master, To_String (Local_Mst));
            MDec_Exc := To_Unbounded_String (Next_Label (Ctx.E, "mdec_exc"));
            Mst_Exc  := To_Unbounded_String (Next_Label (Ctx.E, "mst_exc"));
            Mst_Done := To_Unbounded_String (Next_Label (Ctx.E, "mst_done"));
            -- If an exception occurs during declaration elaboration, abort unactivated tasks and re-raise (LRM 9.3)
            Ctx.Handlers.Append ((Handler_Label => MDec_Exc,
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));
         end if;

         declare
            Block_Has_Stubs : Boolean := False;
            Saved_Has_Stubs : constant Boolean := Ctx.Has_Stubs;
         begin
            if Is_Subp and then not Is_Void_Tree (Item_S) then
               declare
                  SL : SEQ_TYPE := LIST (Item_S);
                  IT : TREE;
               begin
                  while not IS_EMPTY (SL) loop
                     POP (SL, IT);
                     if (KIND (IT) = DN_SUBPROGRAM_BODY or else KIND (IT) = DN_PACKAGE_BODY or else KIND (IT) = DN_TASK_BODY)
                       and then not Is_Void_Tree (D (AS_BODY, IT))
                       and then KIND (D (AS_BODY, IT)) = DN_STUB
                       and then not Is_Gen_Subunit_Stub (Ctx, IT)
                     then
                        Block_Has_Stubs := True;
                        exit;
                     end if;
                  end loop;
               end;
            end if;
            if Block_Has_Stubs then
               Ctx.Has_Stubs := True;
            end if;
            Emit_Decl_Items (Ctx, Item_S, Is_Global => Block_Has_Stubs, Export_All => Block_Has_Stubs, Has_Dyn => Has_Dyn);
            Ctx.Has_Stubs := Saved_Has_Stubs;
         end;

         if Has_Tasks then
            Ctx.Handlers.Delete_Last;
            -- Outer cleanup handler so ANY unhandled exception leaving the block waits for dependent tasks (LRM 9.4)
            Ctx.Handlers.Append ((Handler_Label => Mst_Exc,
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));
         end if;

         if Has_Hdl then
            Hdl_Lbl   := To_Unbounded_String (Next_Label (Ctx.E, "exc_h"));
            After_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "exc_end"));
            declare
               H_Mark : Unbounded_String := Null_Unbounded_String;
               H_SP   : constant String := Next_Reg (Ctx.E, "hsp");
            begin
               -- Take a fresh SS mark & SP mark AFTER Emit_Decl_Items so entering an exception handler
               -- never frees local dynamic/unconstrained arrays declared in this block/subprogram!
               Emit_Inst (Ctx.E, H_SP & " = stacksave");
               if not Ctx.Cur_Ret_Uncon and then Tree_Uses_SS (Ctx, Stm_S) then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", H_Mark, Nounwind => True);
               end if;
               Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                     Current_Exn   => Null_Unbounded_String,
                                     In_Handler    => False,
                                     SS_Mark       => H_Mark,
                                     SP_Mark       => To_Unbounded_String (H_SP)));
            end;
         end if;

         -- Activate all dependent tasks declared in this master scope right at `begin` (LRM 9.3)
         if Has_Tasks then
            declare
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate", To_String (Local_Mst), "", Dummy);
            end;
         end if;

         Emit_Stm_List (Ctx, Stm_S);

         if Has_Hdl then
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & To_String (After_Lbl));
            end if;
            Emit_Exception_Handlers (Ctx, Alt_S, To_String (Hdl_Lbl), To_String (After_Lbl));
            Ctx.Handlers.Delete_Last;
            Emit_Label (Ctx.E, To_String (After_Lbl));
         end if;

         if Has_Tasks then
            Ctx.Handlers.Delete_Last;
            if not Is_Terminated (Ctx.E) then
               declare
                  Dummy : Unbounded_String;
               begin
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (Local_Mst), "", Dummy);
                  Emit_Term (Ctx.E, "jmp " & To_String (Mst_Done));
               end;
            end if;
            declare
               Dex_R : constant String := Next_Reg (Ctx.E, "mdex");
               Mex_R : constant String := Next_Reg (Ctx.E, "msex");
               Dummy : Unbounded_String;
            begin
               Emit_Label (Ctx.E, To_String (MDec_Exc), Dex_R & ": ptr");
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_unactivated", To_String (Local_Mst), "", Dummy, Nounwind => True);
               Emit_Raise_Reg (Ctx, Dex_R);

               Emit_Label (Ctx.E, To_String (Mst_Exc), Mex_R & ": ptr");
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (Local_Mst), "", Dummy, Nounwind => True);
               Emit_Raise_Reg (Ctx, Mex_R);

               Emit_Label (Ctx.E, To_String (Mst_Done));
            end;
            Ctx.Cur_Master_Reg := Saved_Mst;
            Pop_Cleanup (Ctx, CK_Master);
         end if;
      end;

      if Has_SS then
         if not Is_Terminated (Ctx.E) then
            declare
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (Saved_SS), "", Dummy, Nounwind => True);
            end;
         end if;
         Pop_Cleanup (Ctx, CK_SS_Mark);
      end if;

      if not Is_Subp then
         if Has_Dyn and then not Is_Terminated (Ctx.E) then
            Emit_Inst (Ctx.E, "stackrestore " & To_String (Saved_SP));
         end if;
         Pop_Cleanup (Ctx, CK_SP_Mark);
      end if;
   end Emit_Block_Body;

   procedure Emit_Case_Stm (Ctx : in out Context; Stm : TREE) is
      Sel    : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Stm));
      PT     : constant String := To_String (Sel.Pir_Type);
      Alts   : SEQ_TYPE := LIST (D (AS_ALTERNATIVE_S, Stm));
      Alt    : TREE;
      L_End  : constant String := Next_Label (Ctx.E, "cas_end");
      L_Def  : constant String := Next_Label (Ctx.E, "cas_def");

      type Case_Arm is record
         Lbl  : Unbounded_String;
         Node : TREE;
      end record;
      package Arm_Vecs is new Ada.Containers.Vectors (Natural, Case_Arm);
      Arms     : Arm_Vecs.Vector;
      Sw_Cases : Unbounded_String;
      Has_Sw   : Boolean := False;
      Def_Arm  : TREE := CONST_VOID;

      type Wide_Range is record
         Lo  : Integer_64;
         Hi  : Integer_64;
         Lbl : Unbounded_String;
      end record;
      package WR_Vecs is new Ada.Containers.Vectors (Natural, Wide_Range);
      Wide_Ranges : WR_Vecs.Vector;
   begin
      while not IS_EMPTY (Alts) loop
         POP (Alts, Alt);
         if KIND (Alt) = DN_ALTERNATIVE then
            declare
               AL  : constant String := Next_Label (Ctx.E, "cas_a");
               CS  : SEQ_TYPE := LIST (D (AS_CHOICE_S, Alt));
               Ch  : TREE;
               Is_Oth : Boolean := False;
            begin
               while not IS_EMPTY (CS) loop
                  POP (CS, Ch);
                  if KIND (Ch) = DN_CHOICE_OTHERS then
                     Is_Oth := True;
                     Def_Arm := Alt;
                  elsif KIND (Ch) = DN_CHOICE_EXP then
                     declare
                        V : constant Integer_64 := Eval_Int64 (Safe_SM_Value (D (AS_EXP, Ch)));
                     begin
                        if Has_Sw then Append (Sw_Cases, ", "); end if;
                        Has_Sw := True;
                        Append (Sw_Cases, Img (Long_Long_Integer (V)) & " -> " & AL);
                     end;
                  elsif KIND (Ch) = DN_CHOICE_RANGE then
                     declare
                        DR : constant TREE := D (AS_DISCRETE_RANGE, Ch);
                        Lo, Hi : Integer_64 := 0;
                        St : Boolean := False;
                     begin
                        if KIND (DR) = DN_RANGE then
                           if Has_Static_Val (Safe_SM_Value (D (AS_EXP1, DR))) and then
                              Has_Static_Val (Safe_SM_Value (D (AS_EXP2, DR)))
                           then
                              Lo := Eval_Int64 (Safe_SM_Value (D (AS_EXP1, DR)));
                              Hi := Eval_Int64 (Safe_SM_Value (D (AS_EXP2, DR)));
                              St := True;
                           end if;
                        elsif KIND (DR) = DN_DISCRETE_SUBTYPE then
                           declare
                              SI : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                           begin
                              if KIND (SI) = DN_SUBTYPE_INDICATION and then not Is_Void_Tree (D (AS_CONSTRAINT, SI))
                                and then KIND (D (AS_CONSTRAINT, SI)) = DN_RANGE
                              then
                                 declare
                                    R : constant TREE := D (AS_CONSTRAINT, SI);
                                 begin
                                    if Has_Static_Val (Safe_SM_Value (D (AS_EXP1, R))) and then
                                       Has_Static_Val (Safe_SM_Value (D (AS_EXP2, R)))
                                    then
                                       Lo := Eval_Int64 (Safe_SM_Value (D (AS_EXP1, R)));
                                       Hi := Eval_Int64 (Safe_SM_Value (D (AS_EXP2, R)));
                                       St := True;
                                    end if;
                                 end;
                              else
                                 declare
                                    Def : constant TREE :=
                                      (if KIND (SI) = DN_SUBTYPE_INDICATION then Get_Name_Defn (D (AS_NAME, SI))
                                       else Get_Name_Defn (SI));
                                 begin
                                    if not Is_Void_Tree (Def) and then KIND (Def) in CLASS_TYPE_NAME then
                                       Get_Discrete_Bounds (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def)), St, Lo, Hi);
                                    end if;
                                 end;
                              end if;
                           end;
                        elsif KIND (DR) = DN_RANGE_ATTRIBUTE then
                           declare
                              Pfx_N : constant TREE := D (AS_NAME, DR);
                              Dim_E : constant TREE := D (AS_EXP, DR);
                              Dim_N : Natural := 1;
                              Pfx_T : TREE := CONST_VOID;
                           begin
                              if not Is_Void_Tree (Dim_E) and then Has_Static_Val (Safe_SM_Value (Dim_E)) then
                                 Dim_N := Natural'Max (1, Natural (Eval_Int64 (Safe_SM_Value (Dim_E))));
                              end if;
                              if not Is_Void_Tree (Pfx_N) then
                                 declare
                                    PDef : constant TREE := Get_Name_Defn (Pfx_N);
                                 begin
                                    if not Is_Void_Tree (PDef) and then KIND (PDef) in CLASS_TYPE_NAME then
                                       Pfx_T := Subst_Resolve_Type (Ctx, D (SM_TYPE_SPEC, PDef));
                                    elsif KIND (Pfx_N) in CLASS_EXP then
                                       Pfx_T := Subst_Resolve_Type (Ctx, D (SM_EXP_TYPE, Pfx_N));
                                    elsif not Is_Void_Tree (PDef) and then KIND (PDef) in CLASS_OBJECT_NAME then
                                       Pfx_T := Subst_Resolve_Type (Ctx, D (SM_OBJ_TYPE, PDef));
                                    end if;
                                 end;
                              end if;
                              if not Is_Void_Tree (Pfx_T) and then KIND (Pfx_T) = DN_CONSTRAINED_ARRAY
                                and then not Is_Void_Tree (D (SM_INDEX_SUBTYPE_S, Pfx_T))
                              then
                                 declare
                                    IL   : SEQ_TYPE := LIST (D (SM_INDEX_SUBTYPE_S, Pfx_T));
                                    IdxT : TREE := CONST_VOID;
                                    K_D  : Natural := 0;
                                 begin
                                    while not IS_EMPTY (IL) and then K_D < Dim_N loop
                                       POP (IL, IdxT);
                                       K_D := K_D + 1;
                                    end loop;
                                    if K_D = Dim_N and then not Is_Void_Tree (IdxT) then
                                       Get_Discrete_Bounds (Subst_Tree (Ctx, IdxT), St, Lo, Hi);
                                    end if;
                                 end;
                              end if;
                           end;
                        end if;
                        if St then
                           if Hi >= Lo and then Lo <= Integer_64'Last - 64 and then Hi <= Lo + 64 then
                              for V in Lo .. Hi loop
                                 if Has_Sw then Append (Sw_Cases, ", "); end if;
                                 Has_Sw := True;
                                 Append (Sw_Cases, Img (Long_Long_Integer (V)) & " -> " & AL);
                              end loop;
                           elsif Hi >= Lo then
                              Wide_Ranges.Append ((Lo => Lo, Hi => Hi, Lbl => To_Unbounded_String (AL)));
                           end if;
                        end if;
                     end;
                  end if;
               end loop;
               if not Is_Oth then
                  Arms.Append ((Lbl => To_Unbounded_String (AL), Node => Alt));
               end if;
            end;
         end if;
      end loop;

      if Has_Sw then
         Emit_Term (Ctx.E,
           "switch." & PT & " " & To_String (Sel.Reg) & ", default " & L_Def &
           " [ " & To_String (Sw_Cases) & " ]");
      else
         Emit_Term (Ctx.E, "jmp " & L_Def);
      end if;

      for A of Arms loop
         Emit_Label (Ctx.E, To_String (A.Lbl));
         Emit_Stm_List (Ctx, D (AS_STM_S, A.Node));
         if not Is_Terminated (Ctx.E) then
            Emit_Term (Ctx.E, "jmp " & L_End);
         end if;
      end loop;

      Emit_Label (Ctx.E, L_Def);
      declare
         Is_Uns : constant Boolean := Is_Unsigned_Scalar (Subst_Tree (Ctx, Sel.Ada_Type), PT);
         Ge_Op  : constant String := (if Is_Uns then "cmp_ge_u." else "cmp_ge_s.");
         Le_Op  : constant String := (if Is_Uns then "cmp_le_u." else "cmp_le_s.");
      begin
         for WR of Wide_Ranges loop
            declare
               C1   : constant String := Next_Reg (Ctx.E, "wc1");
               C2   : constant String := Next_Reg (Ctx.E, "wc2");
               InR  : constant String := Next_Reg (Ctx.E, "win");
               Next : constant String := Next_Label (Ctx.E, "wnx");
            begin
               Emit_Inst (Ctx.E, C1 & " = " & Ge_Op & PT & " " & To_String (Sel.Reg) & ", " & Img (Long_Long_Integer (WR.Lo)));
               Emit_Inst (Ctx.E, C2 & " = " & Le_Op & PT & " " & To_String (Sel.Reg) & ", " & Img (Long_Long_Integer (WR.Hi)));
               Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
               Emit_Term (Ctx.E, "br " & InR & ", " & To_String (WR.Lbl) & ", " & Next);
               Emit_Label (Ctx.E, Next);
            end;
         end loop;
      end;

      if not Is_Void_Tree (Def_Arm) then
         Emit_Stm_List (Ctx, D (AS_STM_S, Def_Arm));
      end if;
      if not Is_Terminated (Ctx.E) then
         Emit_Term (Ctx.E, "jmp " & L_End);
      end if;
      Emit_Label (Ctx.E, L_End);
   end Emit_Case_Stm;

   procedure Emit_Loop_Stm (Ctx : in out Context; Stm : TREE) is
      Iter      : constant TREE := D (AS_ITERATION, Stm);
      Loop_Id   : constant TREE := D (AS_SOURCE_NAME, Stm);
      L_Key     : constant Unsigned_32 := (if Is_Void_Tree (Loop_Id) then 0 else Tree_Key (Loop_Id));
      L_End     : constant String := Next_Label (Ctx.E, "lp_end");
      Body_Stms : constant TREE := D (AS_STM_S, Stm);
      Cond_SS   : constant Boolean :=
        not Is_Void_Tree (Iter) and then KIND (Iter) = DN_WHILE and then Tree_Uses_SS (Ctx, D (AS_EXP, Iter));
      Loop_SS   : constant Boolean := Tree_Uses_SS (Ctx, Body_Stms) or else Cond_SS;

      procedure Emit_Body_With_SS is
         Mark  : Unbounded_String;
         Dummy : Unbounded_String;
      begin
         if Loop_SS then
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", Mark, Nounwind => True);
            Push_Cleanup (Ctx, CK_SS_Mark, To_String (Mark));
         end if;
         Emit_Stm_List (Ctx, Body_Stms);
         if Loop_SS then
            if not Is_Terminated (Ctx.E) then
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (Mark), "", Dummy, Nounwind => True);
            end if;
            Pop_Cleanup (Ctx, CK_SS_Mark);
         end if;
      end Emit_Body_With_SS;
   begin
      Ctx.Loops.Append ((Label_Key     => L_Key,
                         Exit_Label    => To_Unbounded_String (L_End),
                         Cleanup_Depth => Cleanup_Depth (Ctx)));

      if Is_Void_Tree (Iter) then
         -- Infinite loop
         declare
            L_Top : constant String := Next_Label (Ctx.E, "lp_top");
         begin
            Emit_Term (Ctx.E, "jmp " & L_Top);
            Emit_Label (Ctx.E, L_Top);
            Emit_Body_With_SS;
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & L_Top);
            end if;
         end;

      elsif KIND (Iter) = DN_WHILE then
         declare
            L_Cond : constant String := Next_Label (Ctx.E, "wh_c");
            L_Body : constant String := Next_Label (Ctx.E, "wh_b");
         begin
            Emit_Term (Ctx.E, "jmp " & L_Cond);
            Emit_Label (Ctx.E, L_Cond);
            declare
               CMark : Unbounded_String;
               Dummy : Unbounded_String;
            begin
               if Cond_SS then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", CMark, Nounwind => True);
               end if;
               declare
                  C  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Iter));
                  CR : constant String := Emit_Scalar_Coerce (Ctx, To_String (C.Reg), To_String (C.Pir_Type), "i8");
               begin
                  if Cond_SS then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (CMark), "", Dummy, Nounwind => True);
                  end if;
                  Emit_Term (Ctx.E, "br " & CR & ", " & L_Body & ", " & L_End);
               end;
            end;
            Emit_Label (Ctx.E, L_Body);
            Emit_Body_With_SS;
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & L_Cond);
            end if;
         end;

      elsif KIND (Iter) = DN_FOR or else KIND (Iter) = DN_REVERSE then
         declare
            Is_Rev   : constant Boolean := KIND (Iter) = DN_REVERSE;
            Iter_Id  : constant TREE := D (AS_SOURCE_NAME, Iter);
            Rng_Node : constant TREE := D (AS_DISCRETE_RANGE, Iter);
            PT, Lo_R, Hi_R : Unbounded_String;
            Start_R, Stop_R : Unbounded_String;
            Empty_C  : constant String := Next_Reg (Ctx.E, "femp");
            L_Body   : constant String := Next_Label (Ctx.E, "for_b");
            L_Step   : constant String := Next_Label (Ctx.E, "for_s");
            Var_Reg  : constant String := Next_Reg (Ctx.E, "iv");
            Cur_V    : constant String := Next_Reg (Ctx.E, "fv");
            Done_C   : constant String := Next_Reg (Ctx.E, "fdone");
            Next_V   : constant String := Next_Reg (Ctx.E, "fnx");
            Ada_T    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Iter_Id));
            TI       : constant Type_Info := Get_Type_Info (Ctx, Ada_T);
            VInfo    : Var_Info;
         begin
            Emit_Discrete_Range (Ctx, Rng_Node, PT, Lo_R, Hi_R);
            -- Allocate loop variable in .entry (promotable to SSA register by mem2reg!)
            Emit_Entry_Alloca (Ctx.E, Var_Reg & " = alloca " & To_String (PT) & ", align " & Img (TI.Align));
            VInfo.Kind       := VK_Local_Alloca;
            VInfo.Pir_Name   := To_Unbounded_String (Var_Reg);
            VInfo.Pir_Type   := PT;
            VInfo.Ada_Type   := Ada_T;
            VInfo.Align      := TI.Align;
            VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
            -- Record static loop bounds on read-only Ada loop parameter for sound bounds-check elimination!
            if KIND (Rng_Node) = DN_RANGE then
               declare
                  V1 : constant TREE := Safe_SM_Value (D (AS_EXP1, Rng_Node));
                  V2 : constant TREE := Safe_SM_Value (D (AS_EXP2, Rng_Node));
               begin
                  if Has_Static_Val (V1) and then Has_Static_Val (V2) then
                     VInfo.Has_Static_Rng := True;
                     VInfo.Rng_Lo := Eval_Int64 (V1);
                     VInfo.Rng_Hi := Eval_Int64 (V2);
                  end if;
               end;
            end if;
            if Ctx.Vars.Contains (Tree_Key (Iter_Id)) and then Ctx.Vars.Element (Tree_Key (Iter_Id)).Owner_Subp = Ctx.Cur_Subp_Key then
               VInfo.Captured_Slot := Ctx.Vars.Element (Tree_Key (Iter_Id)).Captured_Slot;
            end if;
            Ctx.Vars.Include (Tree_Key (Iter_Id), VInfo);
            if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
               declare
                  SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                  FP : constant String := Next_Reg (Ctx.E, "lcfp");
               begin
                  Emit_Inst (Ctx.E,
                    FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                    To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                  Emit_Inst (Ctx.E, "store.ptr " & Var_Reg & ", " & FP & ", align 8");
               end;
            end if;

            Start_R := (if Is_Rev then Hi_R else Lo_R);
            Stop_R  := (if Is_Rev then Lo_R else Hi_R);

            -- Check if upper/lower bound is statically safe from wrap (enables canonical 2-block loop for loop_vec/rotate!)
            declare
               Can_Fast : Boolean := False;
               Limit_V  : Integer_64 := 0;
               Is_Uns   : constant Boolean := Is_Unsigned_Scalar (Ada_T, To_String (PT));
            begin
               if KIND (Rng_Node) = DN_RANGE then
                  declare
                     Bound_N : constant TREE := (if Is_Rev then D (AS_EXP1, Rng_Node) else D (AS_EXP2, Rng_Node));
                     BV      : constant TREE := Safe_SM_Value (Bound_N);
                     Bits    : constant Natural := Scalar_Bits (Ada_T);
                  begin
                     if Has_Static_Val (BV) then
                        Limit_V := Eval_Int64 (BV);
                        if Is_Uns then
                           if not Is_Rev then
                              if (Bits = 8 and then Limit_V < 255)
                                or else (Bits = 16 and then Limit_V < 65535)
                                or else (Bits = 32 and then Limit_V < 2147483647)
                              then
                                 Can_Fast := True;
                              end if;
                           else
                              if Limit_V > 0 then
                                 Can_Fast := True;
                              end if;
                           end if;
                        elsif not Is_Rev then
                           if (Bits = 32 and then Limit_V < 2147483647)
                             or else (Bits = 64 and then Limit_V < Integer_64'Last)
                             or else (Bits = 16 and then Limit_V < 32767)
                             or else (Bits = 8 and then Limit_V < 127)
                           then
                              Can_Fast := True;
                           end if;
                        else
                           if (Bits = 32 and then Limit_V > -2147483648)
                             or else (Bits = 64 and then Limit_V > Integer_64'First)
                             or else (Bits = 16 and then Limit_V > -32768)
                             or else (Bits = 8 and then Limit_V > -128)
                           then
                              Can_Fast := True;
                           end if;
                        end if;
                     end if;
                  end;
               end if;

               if Can_Fast then
                  declare
                     L_Hdr  : constant String := Next_Label (Ctx.E, "for_h");
                     Cmp_C  : constant String := Next_Reg (Ctx.E, "fcmp");
                     Cmp_Op : constant String :=
                       (if Is_Uns then (if Is_Rev then "cmp_ge_u." else "cmp_le_u.")
                        else (if Is_Rev then "cmp_ge_s." else "cmp_le_s."));
                     Step_Fl : constant String := (if Is_Uns then "" else " [unsafe_s]");
                  begin
                     Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & To_String (Start_R) & ")");
                     Emit_Label (Ctx.E, L_Hdr, Cur_V & ": " & To_String (PT));
                     Emit_Inst (Ctx.E, Cmp_C & " = " & Cmp_Op & To_String (PT) & " " & Cur_V & ", " & To_String (Stop_R));
                     Emit_Term (Ctx.E, "br " & Cmp_C & ", " & L_Body & ", " & L_End);
                     Emit_Label (Ctx.E, L_Body);
                     Emit_Inst (Ctx.E, "store." & To_String (PT) & " " & Cur_V & ", " & Var_Reg & ", align " & Img (TI.Align));
                     Emit_Body_With_SS;
                     if not Is_Terminated (Ctx.E) then
                        if Is_Rev then
                           Emit_Inst (Ctx.E, Next_V & " = sub." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        else
                           Emit_Inst (Ctx.E, Next_V & " = add." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        end if;
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Next_V & ")");
                     end if;
                  end;
               else
                  -- Wrap-safe loop when bound is dynamic or equals Type'Last / Type'First!
                  declare
                     Gt_Op   : constant String := (if Is_Uns then "cmp_gt_u." else "cmp_gt_s.");
                     Step_Fl : constant String := (if Is_Uns then "" else " [unsafe_s]");
                  begin
                     Emit_Inst (Ctx.E, Empty_C & " = " & Gt_Op & To_String (PT) & " " & To_String (Lo_R) & ", " & To_String (Hi_R));
                     Emit_Term (Ctx.E, "br " & Empty_C & ", " & L_End & ", " & L_Body & "(" & To_String (Start_R) & ")");

                     Emit_Label (Ctx.E, L_Body, Cur_V & ": " & To_String (PT));
                     Emit_Inst (Ctx.E, "store." & To_String (PT) & " " & Cur_V & ", " & Var_Reg & ", align " & Img (TI.Align));
                     Emit_Body_With_SS;
                     if not Is_Terminated (Ctx.E) then
                        Emit_Inst (Ctx.E, Done_C & " = cmp_eq." & To_String (PT) & " " & Cur_V & ", " & To_String (Stop_R));
                        Emit_Term (Ctx.E, "br " & Done_C & ", " & L_End & ", " & L_Step);
                        Emit_Label (Ctx.E, L_Step);
                        if Is_Rev then
                           Emit_Inst (Ctx.E, Next_V & " = sub." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        else
                           Emit_Inst (Ctx.E, Next_V & " = add." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        end if;
                        Emit_Term (Ctx.E, "jmp " & L_Body & "(" & Next_V & ")");
                     end if;
                  end;
               end if;
            end;
         end;
      end if;

      Ctx.Loops.Delete_Last;
      Emit_Label (Ctx.E, L_End);
   end Emit_Loop_Stm;

   procedure Emit_Stm (Ctx : in out Context; Stm : TREE) is
      K : constant NODE_NAME := KIND (Stm);
   begin
      case K is
         when DN_NULL_STM | DN_STM_PRAGMA =>
            null;

         when DN_LABELED =>
            declare
               LS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Stm));
               LID : TREE;
            begin
               while not IS_EMPTY (LS) loop
                  POP (LS, LID);
                  declare
                     Lbl : constant String := ".ada_lbl_" & Tree_Key_Str (LID) & To_String (Ctx.Gen_Inst_Tag);
                  begin
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "jmp " & Lbl);
                     end if;
                     Emit_Label (Ctx.E, Lbl);
                  end;
               end loop;
               Emit_Stm (Ctx, D (AS_STM, Stm));
            end;

         when DN_GOTO =>
            declare
               LDef : constant TREE := Get_Name_Defn (D (AS_NAME, Stm));
               LK   : constant Unsigned_32 := Tree_Key (LDef);
            begin
               if Ctx.Label_Cleanup_Depths.Contains (LK) then
                  Emit_Unwind_To (Ctx, Natural (Ctx.Label_Cleanup_Depths.Element (LK)));
               end if;
               Emit_Term (Ctx.E, "jmp .ada_lbl_" & Tree_Key_Str (LDef) & To_String (Ctx.Gen_Inst_Tag));
            end;

         when DN_ASSIGN =>
            declare
               LHS_Node  : constant TREE := D (AS_NAME, Stm);
               Is_BF_LHS : Boolean := False;
               BF_FE     : Field_Entry;
               BF_Def    : TREE := CONST_VOID;
            begin
               if KIND (LHS_Node) = DN_USED_OBJECT_ID
                 or else (KIND (LHS_Node) = DN_SELECTED
                          and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))
                          and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))))
                                   not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID)
               then
                  declare
                     OD_L : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (LHS_Node)));
                  begin
                     if not Is_Void_Tree (OD_L) and then Ctx.Vars.Contains (Tree_Key (OD_L))
                       and then Ctx.Vars.Element (Tree_Key (OD_L)).Is_Bitfield
                     then
                        Is_BF_LHS := True;
                        BF_FE     := Ctx.Vars.Element (Tree_Key (OD_L)).BF_FE;
                        BF_Def    := OD_L;
                     end if;
                  end;
               elsif KIND (LHS_Node) = DN_SELECTED then
                  declare
                     Des : TREE := D (AS_DESIGNATOR, LHS_Node);
                     Def : TREE;
                  begin
                     while not Is_Void_Tree (Des) and then KIND (Des) = DN_SELECTED loop
                        Des := D (AS_DESIGNATOR, Des);
                     end loop;
                     Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des)));
                     if not Is_Void_Tree (Def) and then KIND (Def) in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                        declare
                           Pfx_E  : constant TREE := D (AS_NAME, LHS_Node);
                           Pfx_Ts : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                           Rec_Ts : constant TREE :=
                             (if Is_Access_Type (Pfx_Ts) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_Ts)))
                              else Pfx_Ts);
                           Dummy_RTI : constant Type_Info := Get_Type_Info (Ctx, Rec_Ts);
                           pragma Unreferenced (Dummy_RTI);
                           CK : constant Unsigned_32 := Tree_Key (Def);
                        begin
                           if Ctx.Fields.Contains (CK) and then Is_Bitfield (Ctx.Fields.Element (CK)) then
                              declare
                                 Comp_T : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def));
                              begin
                                 if Get_Type_Info (Ctx, Comp_T).Kind = TK_Scalar then
                                    Is_BF_LHS := True;
                                    BF_FE     := Ctx.Fields.Element (CK);
                                    BF_Def    := Def;
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                  end;
               end if;
               if Is_BF_LHS then
                  declare
                     Is_Ren_BF : constant Boolean := KIND (BF_Def) not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID;
                     Rec_P  : Unbounded_String;
                     BF_CT  : constant TREE :=
                       (if Is_Ren_BF then Ctx.Vars.Element (Tree_Key (BF_Def)).Ada_Type
                        else Subst_Tree (Ctx, D (SM_OBJ_TYPE, BF_Def)));
                     DTI    : constant Type_Info := Get_Type_Info (Ctx, BF_CT);
                     RHS    : Expr_Val;
                     VR     : Unbounded_String;
                  begin
                     if Is_Ren_BF then
                        Rec_P := To_Unbounded_String (Emit_Var_Addr (Ctx, Ctx.Vars.Element (Tree_Key (BF_Def))));
                     else
                        declare
                           Pfx_LV : constant Expr_Val := Emit_LValue (Ctx, D (AS_NAME, LHS_Node));
                           Pfx_T  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Pfx_LV.Ada_Type));
                        begin
                           Rec_P := Pfx_LV.Reg;
                           if Is_Access_Type (Pfx_T) then
                              if Pfx_LV.Kind = EK_Addr then
                                 declare
                                    LP : constant String := Next_Reg (Ctx.E, "bfap");
                                 begin
                                    Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_P) & ", align 8");
                                    Rec_P := To_Unbounded_String (LP);
                                 end;
                              end if;
                              Emit_Null_Check (Ctx, To_String (Rec_P));
                           end if;
                           -- P3-6.3: Run variant discriminant check before bitfield store (LRM 4.1.3(16))
                           if BF_FE.In_Union then
                              declare
                                 Rec_T_BF : constant TREE :=
                                   (if Is_Access_Type (Pfx_T) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T)))
                                    else Pfx_T);
                              begin
                                 Emit_Variant_Check_For_Field (Ctx, To_String (Rec_P), Rec_T_BF, BF_Def, Tree_Key (BF_Def));
                              end;
                           end if;
                        end;
                     end if;
                     RHS := Emit_Expr (Ctx, D (AS_EXP, Stm), BF_CT);
                     declare
                        DP : constant String := To_String (DTI.Pir_Type);
                     begin
                        VR := To_Unbounded_String
                          (Emit_Typed_Scalar_Convert
                             (Ctx, To_String (RHS.Reg), RHS.Ada_Type, To_String (RHS.Pir_Type),
                              BF_CT, DP, Check_Range => True));
                        Emit_Bitfield_Store (Ctx, To_String (Rec_P), BF_FE, To_String (VR), DP, BF_CT);
                     end;
                  end;
               else
                  declare
                     LHS       : constant Expr_Val := Emit_LValue (Ctx, LHS_Node);
                     DTI       : constant Type_Info := Get_Type_Info (Ctx, LHS.Ada_Type);
                     RHS_Node  : constant TREE := D (AS_EXP, Stm);
                     Is_Shared : constant Boolean := Is_Shared_Object (Ctx, LHS_Node);
                  begin
               if DTI.Kind = TK_Scalar then
                  declare
                     RHS : constant Expr_Val := Emit_Expr (Ctx, RHS_Node, LHS.Ada_Type);
                     SP  : constant String := To_String (RHS.Pir_Type);
                     DP  : constant String := To_String (DTI.Pir_Type);
                     VR  : Unbounded_String;
                     -- P3-3.13: If LHS is an object with its own per-object frozen range (multi-object decl),
                     -- bind its frozen range into Saved_Type_Ranges for the duration of Emit_Range_Check!
                     Swap_TK  : Unsigned_32 := 0;
                     Had_Orig : Boolean := False;
                     Orig_SR  : Saved_Range_Info;
                  begin
                     if KIND (LHS_Node) = DN_USED_OBJECT_ID
                       or else (KIND (LHS_Node) = DN_SELECTED
                                and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))
                                and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))))
                                         not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID)
                     then
                        declare
                           OD_A : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (LHS_Node)));
                           OK_A : constant Unsigned_32 := (if not Is_Void_Tree (OD_A) then Tree_Key (OD_A) else 0);
                           RT_A : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, LHS.Ada_Type));
                        begin
                           if OK_A /= 0 and then Ctx.Saved_Var_Ranges.Contains (OK_A) and then not Is_Void_Tree (RT_A) then
                              Swap_TK := Tree_Key (RT_A);
                              if Ctx.Saved_Type_Ranges.Contains (Swap_TK) then
                                 Had_Orig := True;
                                 Orig_SR  := Ctx.Saved_Type_Ranges.Element (Swap_TK);
                              end if;
                              Ctx.Saved_Type_Ranges.Include (Swap_TK, Ctx.Saved_Var_Ranges.Element (OK_A));
                           end if;
                        end;
                     end if;
                     VR := To_Unbounded_String
                       (Emit_Typed_Scalar_Convert
                          (Ctx, To_String (RHS.Reg), RHS.Ada_Type, SP,
                           LHS.Ada_Type, DP, Check_Range => True));
                     if Swap_TK /= 0 then
                        if Had_Orig then
                           Ctx.Saved_Type_Ranges.Include (Swap_TK, Orig_SR);
                        else
                           Ctx.Saved_Type_Ranges.Exclude (Swap_TK);
                        end if;
                     end if;
                     Emit_Discriminant_Check (Ctx, To_String (VR), LHS.Ada_Type, Is_Access => True);
                     if Is_Shared then
                        Emit_Inst (Ctx.E, "fence [seq_cst]");
                        Emit_Inst (Ctx.E,
                          "store." & DP & " " & To_String (VR) &
                          ", " & To_String (LHS.Reg) & ", align " & Img (DTI.Align) & " [volatile]");
                        Emit_Inst (Ctx.E, "fence [seq_cst]");
                     else
                        Emit_Inst (Ctx.E,
                          "store." & DP & " " & To_String (VR) &
                          ", " & To_String (LHS.Reg) & ", align " & Img (DTI.Align));
                     end if;
                  end;
               else
                  declare
                     -- Check if RHS is an aggregate containing `others` (within array dims only!) assigned to an
                     -- unconstrained/dynamic array LHS (needs LHS's bounds in a temp buffer
                     -- to preserve LRM 5.2 exception safety & non-aliasing!).
                     function Agg_Has_Others (AN : TREE; Rem_D : Natural) return Boolean is
                     begin
                        if Rem_D = 0 or else Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then
                           return False;
                        end if;
                        declare
                           AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                           IT : TREE;
                        begin
                           while not IS_EMPTY (AL) loop
                              POP (AL, IT);
                              if KIND (IT) = DN_ASSOC or else KIND (IT) = DN_NAMED then
                                 declare
                                    CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                    Ch : TREE;
                                 begin
                                    while not IS_EMPTY (CS) loop
                                       POP (CS, Ch);
                                       if KIND (Ch) = DN_CHOICE_OTHERS then
                                          return True;
                                       end if;
                                    end loop;
                                 end;
                                 if Rem_D > 1 and then Agg_Has_Others (D (AS_EXP, IT), Rem_D - 1) then
                                    return True;
                                 end if;
                              else
                                 if Rem_D > 1 and then Agg_Has_Others (IT, Rem_D - 1) then
                                    return True;
                                 end if;
                              end if;
                           end loop;
                        end;
                        return False;
                     end Agg_Has_Others;

                     RHS : Expr_Val;
                     Is_Null_Str : Boolean := False;
                  begin
                     if KIND (RHS_Node) = DN_AGGREGATE
                       and then (DTI.Kind = TK_Unconstrained_Array
                                 or else (DTI.Kind = TK_Constrained_Array and then DTI.Is_Dynamic))
                       and then Agg_Has_Others (RHS_Node, Natural'Max (1, DTI.NDims))
                     then
                        -- Allocate temp buffer with LHS's bounds, initialize temp, then copy to LHS
                        declare
                           ETI   : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
                           EAl   : constant Natural := Elem_Storage_Align (ETI);
                           ESz   : constant Natural := Elem_Storage_Size (ETI);
                           ND    : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
                           LFs, LLs : Str_Array_4;
                           Tot64 : Unbounded_String;
                           Byt   : Unbounded_String;
                           Buf_U : Unbounded_String;
                        begin
                           for D_I in 1 .. ND loop
                              Get_Array_Dim_Bounds (Ctx, LHS, D_I, LFs (D_I), LLs (D_I));
                           end loop;
                           Emit_Array_Byte_Size_I64 (Ctx, LFs, LLs, ND, ESz, Tot64, Byt);
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", To_String (Byt) & ", " & Img (EAl), "ptr", Buf_U);
                           Emit_Init_Composite (Ctx, To_String (Buf_U), LHS.Ada_Type, RHS_Node, LFs, LLs);
                           RHS.Kind       := EK_Slice;
                           RHS.Reg        := Buf_U;
                           RHS.Pir_Type   := To_Unbounded_String ("ptr");
                           RHS.Ada_Type   := LHS.Ada_Type;
                           RHS.Align      := EAl;
                           RHS.NDims      := ND;
                           RHS.First_Regs := LFs;
                           RHS.Last_Regs  := LLs;
                        end;
                     elsif KIND (RHS_Node) = DN_STRING_LITERAL
                       and then Sym_Name (RHS_Node) = """"""
                       and then (DTI.Kind = TK_Constrained_Array or else DTI.Kind = TK_Unconstrained_Array)
                     then
                        -- Null string literal "" assigned to array/slice LHS (LRM 4.2(4))
                        Is_Null_Str := True;
                        declare
                           Z1 : constant String := Next_Reg (Ctx.E, "nsf");
                           Z2 : constant String := Next_Reg (Ctx.E, "nsl");
                           ZP : constant String := Next_Reg (Ctx.E, "nsp");
                        begin
                           Emit_Inst (Ctx.E, Z1 & " = const.i64 1");
                           Emit_Inst (Ctx.E, Z2 & " = const.i64 0");
                           Emit_Inst (Ctx.E, ZP & " = const.ptr 0");
                           RHS.Kind := EK_Slice;
                           RHS.Reg := To_Unbounded_String (ZP);
                           RHS.Pir_Type := To_Unbounded_String ("ptr");
                           RHS.Ada_Type := LHS.Ada_Type;
                           RHS.NDims := 1;
                           RHS.First_Regs (1) := To_Unbounded_String (Z1);
                           RHS.Last_Regs (1) := To_Unbounded_String (Z2);
                        end;
                     else
                        RHS := Emit_Expr (Ctx, RHS_Node, LHS.Ada_Type);
                     end if;

                     -- Length check across ALL dimensions (with Ada 83 null-to-null allowance)
                     if DTI.Kind = TK_Constrained_Array or else DTI.Kind = TK_Unconstrained_Array then
                        Emit_Array_Length_Check (Ctx, LHS, RHS, Natural'Max (1, DTI.NDims));
                     end if;
                     if DTI.Kind = TK_Record then
                        Emit_Discriminant_Check (Ctx, To_String (RHS.Reg), LHS.Ada_Type, Is_Access => False);
                        -- LRM 3.7.2(4) / 5.2(4) (P3-3.1, P3-3.3): DN_ALL dereference, constrained record components
                        -- (including discriminant-dependent O.Inner := Bad!), records without default discriminants,
                        -- and constrained IN OUT/OUT/renamed records must preserve LHS's existing discriminants!
                        if Ctx.Enable_Checks then
                           declare
                              LHS_RT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, LHS.Ada_Type));
                              BT : constant TREE := Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, LHS.Ada_Type)));
                              DS : constant TREE := (if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then D (SM_DISCRIMINANT_S, BT) else CONST_VOID);
                              Must_Match : Boolean :=
                                KIND (D (AS_NAME, Stm)) = DN_ALL
                                or else (not Is_Void_Tree (LHS_RT) and then KIND (LHS_RT) = DN_CONSTRAINED_RECORD);
                              Dyn_Con_Reg : Unbounded_String;
                           begin
                              if not Must_Match
                                and then (KIND (D (AS_NAME, Stm)) = DN_USED_OBJECT_ID
                                          or else (KIND (D (AS_NAME, Stm)) = DN_SELECTED
                                                   and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, D (AS_NAME, Stm))))
                                                   and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, D (AS_NAME, Stm))))))
                                                            not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID))
                              then
                                 declare
                                    LDef : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, Stm))));
                                 begin
                                    if not Is_Void_Tree (LDef) and then Ctx.Vars.Contains (Tree_Key (LDef)) then
                                       declare
                                          LVInfo : constant Var_Info := Ctx.Vars.Element (Tree_Key (LDef));
                                       begin
                                          if LVInfo.Owner_Subp = Ctx.Cur_Subp_Key and then Length (LVInfo.Constrained_Reg) > 0 then
                                             Dyn_Con_Reg := LVInfo.Constrained_Reg;
                                          elsif Length (LVInfo.Con_Global) > 0 then
                                             declare
                                                CGA : constant String := Next_Reg (Ctx.E, "dcga");
                                                CGV : constant String := Next_Reg (Ctx.E, "dcgv");
                                             begin
                                                Emit_Inst (Ctx.E, CGA & " = addr " & To_String (LVInfo.Con_Global));
                                                Emit_Inst (Ctx.E, CGV & " = load.i8 " & CGA & ", align 1");
                                                Dyn_Con_Reg := To_Unbounded_String (CGV);
                                             end;
                                          elsif LVInfo.Con_Captured_Slot > 0 and then LVInfo.Owner_Subp /= 0 and then LVInfo.Owner_Subp /= Ctx.Cur_Subp_Key then
                                             declare
                                                CFP : constant String := Emit_Captured_Slot_Ptr (Ctx, LVInfo.Owner_Subp, LVInfo.Con_Captured_Slot);
                                                CCP : constant String := Next_Reg (Ctx.E, "dccp");
                                                CCV : constant String := Next_Reg (Ctx.E, "dccv");
                                             begin
                                                Emit_Inst (Ctx.E, CCP & " = load.ptr " & CFP & ", align 8");
                                                Emit_Inst (Ctx.E, CCV & " = load.i8 " & CCP & ", align 1");
                                                Dyn_Con_Reg := To_Unbounded_String (CCV);
                                             end;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                              if not Must_Match and then Length (Dyn_Con_Reg) = 0
                                and then not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS))
                              then
                                 -- Records without default discriminants are always constrained
                                 declare
                                    DL : SEQ_TYPE := LIST (DS);
                                    DD : TREE;
                                 begin
                                    POP (DL, DD);
                                    if KIND (DD) = DN_DSCRMT_DECL and then Is_Void_Tree (D (AS_EXP, DD)) then
                                       declare
                                          NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                          DID : TREE;
                                       begin
                                          if not IS_EMPTY (NS) then
                                             POP (NS, DID);
                                             if Is_Void_Tree (D (SM_INIT_EXP, DID)) then
                                                Must_Match := True;
                                             end if;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                              if (Must_Match or else Length (Dyn_Con_Reg) > 0)
                                and then not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS))
                              then
                                 declare
                                    RTI    : constant Type_Info := Get_Type_Info (Ctx, BT);
                                    DL     : SEQ_TYPE := LIST (DS);
                                    DD     : TREE;
                                    FIdx   : Natural := 0;
                                    L_Skip : Unbounded_String;
                                 begin
                                    if not Must_Match and then Length (Dyn_Con_Reg) > 0 then
                                       declare
                                          L_Do : constant String := Next_Label (Ctx.E, "dcd");
                                       begin
                                          L_Skip := To_Unbounded_String (Next_Label (Ctx.E, "dcs"));
                                          Emit_Term (Ctx.E, "br " & To_String (Dyn_Con_Reg) & ", " & L_Do & ", " & To_String (L_Skip));
                                          Emit_Label (Ctx.E, L_Do);
                                       end;
                                    end if;
                                    while not IS_EMPTY (DL) loop
                                       POP (DL, DD);
                                       if KIND (DD) = DN_DSCRMT_DECL then
                                          declare
                                             NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                             DID : TREE;
                                          begin
                                             while not IS_EMPTY (NS) loop
                                                POP (NS, DID);
                                                declare
                                                   DT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                                   DTI_D : constant Type_Info := Get_Type_Info (Ctx, DT);
                                                   DK_D : constant Unsigned_32 := Tree_Key (Canonical_Def (DID));
                                                   LV, RV : Unbounded_String;
                                                   Neq : constant String := Next_Reg (Ctx.E, "dne");
                                                begin
                                                   if Ctx.Fields.Contains (DK_D) then
                                                      declare
                                                         DFE : constant Field_Entry := Ctx.Fields.Element (DK_D);
                                                      begin
                                                         if Is_Bitfield (DFE) then
                                                            LV := To_Unbounded_String (Emit_Bitfield_Load (Ctx, To_String (LHS.Reg), DFE, DT));
                                                            RV := To_Unbounded_String (Emit_Bitfield_Load (Ctx, To_String (RHS.Reg), DFE, DT));
                                                         elsif DFE.In_Union or else DFE.Has_Explicit_Offset then
                                                            declare
                                                               Off_R : constant String := Next_Reg (Ctx.E, "doff");
                                                               LP    : constant String := Next_Reg (Ctx.E, "dlp");
                                                               RP    : constant String := Next_Reg (Ctx.E, "drp");
                                                               LV0   : constant String := Next_Reg (Ctx.E, "dlv");
                                                               RV0   : constant String := Next_Reg (Ctx.E, "drv");
                                                            begin
                                                               Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (DFE.Byte_Offset));
                                                               Emit_Inst (Ctx.E, LP & " = ptr_add.i64 " & To_String (LHS.Reg) & ", " & Off_R & " [inbounds]");
                                                               Emit_Inst (Ctx.E, LV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & LP & ", align " & Img (Explicit_Field_Align (DFE, DTI_D.Align)));
                                                               Emit_Inst (Ctx.E, RP & " = ptr_add.i64 " & To_String (RHS.Reg) & ", " & Off_R & " [inbounds]");
                                                               Emit_Inst (Ctx.E, RV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & RP & ", align " & Img (Explicit_Field_Align (DFE, DTI_D.Align)));
                                                               LV := To_Unbounded_String (LV0);
                                                               RV := To_Unbounded_String (RV0);
                                                            end;
                                                         else
                                                            declare
                                                               LP  : constant String := Next_Reg (Ctx.E, "dlp");
                                                               RP  : constant String := Next_Reg (Ctx.E, "drp");
                                                               LV0 : constant String := Next_Reg (Ctx.E, "dlv");
                                                               RV0 : constant String := Next_Reg (Ctx.E, "drv");
                                                            begin
                                                               Emit_Inst (Ctx.E, LP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (LHS.Reg) & ", " & Img (DFE.Field_Idx));
                                                               Emit_Inst (Ctx.E, LV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & LP & ", align " & Img (DTI_D.Align));
                                                               Emit_Inst (Ctx.E, RP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (RHS.Reg) & ", " & Img (DFE.Field_Idx));
                                                               Emit_Inst (Ctx.E, RV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & RP & ", align " & Img (DTI_D.Align));
                                                               LV := To_Unbounded_String (LV0);
                                                               RV := To_Unbounded_String (RV0);
                                                            end;
                                                         end if;
                                                      end;
                                                   else
                                                      declare
                                                         LP  : constant String := Next_Reg (Ctx.E, "dlp");
                                                         RP  : constant String := Next_Reg (Ctx.E, "drp");
                                                         LV0 : constant String := Next_Reg (Ctx.E, "dlv");
                                                         RV0 : constant String := Next_Reg (Ctx.E, "drv");
                                                      begin
                                                         Emit_Inst (Ctx.E, LP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (LHS.Reg) & ", " & Img (FIdx));
                                                         Emit_Inst (Ctx.E, LV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & LP & ", align " & Img (DTI_D.Align));
                                                         Emit_Inst (Ctx.E, RP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (RHS.Reg) & ", " & Img (FIdx));
                                                         Emit_Inst (Ctx.E, RV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & RP & ", align " & Img (DTI_D.Align));
                                                         LV := To_Unbounded_String (LV0);
                                                         RV := To_Unbounded_String (RV0);
                                                      end;
                                                   end if;
                                                   Emit_Inst (Ctx.E, Neq & " = cmp_ne." & To_String (DTI_D.Pir_Type) & " " & To_String (LV) & ", " & To_String (RV));
                                                   Emit_Check_Fail_If (Ctx, Neq);
                                                end;
                                                FIdx := FIdx + 1;
                                             end loop;
                                          end;
                                       end if;
                                    end loop;
                                    if Length (L_Skip) > 0 then
                                       Emit_Term (Ctx.E, "jmp " & To_String (L_Skip));
                                       Emit_Label (Ctx.E, To_String (L_Skip));
                                    end if;
                                 end;
                              end if;
                           end;
                        end if;
                     end if;
                     if not Is_Null_Str then
                        Emit_Copy_Composite (Ctx, To_String (LHS.Reg), RHS, LHS.Ada_Type);
                     end if;
                  end;
               end if;
                  end;
               end if;
            end;

         when DN_IF =>
            declare
               Clauses : SEQ_TYPE := LIST (D (AS_TEST_CLAUSE_ELEM_S, Stm));
               Clause  : TREE;
               Else_S  : constant TREE := D (AS_STM_S, Stm);
               L_End   : constant String := Next_Label (Ctx.E, "if_end");
            begin
               while not IS_EMPTY (Clauses) loop
                  POP (Clauses, Clause);
                  if KIND (Clause) = DN_COND_CLAUSE then
                     declare
                        Cond  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Clause));
                        CondR : constant String := Emit_Scalar_Coerce (Ctx, To_String (Cond.Reg), To_String (Cond.Pir_Type), "i8");
                        L_T   : constant String := Next_Label (Ctx.E, "if_t");
                        L_F   : constant String := Next_Label (Ctx.E, "if_f");
                     begin
                        Emit_Term (Ctx.E, "br " & CondR & ", " & L_T & ", " & L_F);
                        Emit_Label (Ctx.E, L_T);
                        Emit_Stm_List (Ctx, D (AS_STM_S, Clause));
                        if not Is_Terminated (Ctx.E) then
                           Emit_Term (Ctx.E, "jmp " & L_End);
                        end if;
                        Emit_Label (Ctx.E, L_F);
                     end;
                  end if;
               end loop;
               Emit_Stm_List (Ctx, Else_S);
               if not Is_Terminated (Ctx.E) then
                  Emit_Term (Ctx.E, "jmp " & L_End);
               end if;
               Emit_Label (Ctx.E, L_End);
            end;

         when DN_CASE =>
            Emit_Case_Stm (Ctx, Stm);

         when DN_LOOP =>
            Emit_Loop_Stm (Ctx, Stm);

         when DN_EXIT =>
            declare
               Target_Loop : constant TREE := D (SM_STM, Stm);
               Cond_Exp    : constant TREE := D (AS_EXP, Stm);
               Exit_Lbl    : Unbounded_String;
               Exit_Depth  : Natural := Cleanup_Depth (Ctx);
            begin
               if not Is_Void_Tree (Target_Loop) then
                  declare
                     LID : constant TREE := D (AS_SOURCE_NAME, Target_Loop);
                     LK  : constant Unsigned_32 := (if Is_Void_Tree (LID) then 0 else Tree_Key (LID));
                  begin
                     for I in reverse 0 .. Natural (Ctx.Loops.Length) - 1 loop
                        if LK /= 0 and then Ctx.Loops.Element (I).Label_Key = LK then
                           Exit_Lbl   := Ctx.Loops.Element (I).Exit_Label;
                           Exit_Depth := Ctx.Loops.Element (I).Cleanup_Depth;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;
               if Length (Exit_Lbl) = 0 and then not Ctx.Loops.Is_Empty then
                  Exit_Lbl   := Ctx.Loops.Last_Element.Exit_Label;
                  Exit_Depth := Ctx.Loops.Last_Element.Cleanup_Depth;
               end if;

               if Is_Void_Tree (Cond_Exp) then
                  Emit_Unwind_To (Ctx, Exit_Depth);
                  Emit_Term (Ctx.E, "jmp " & To_String (Exit_Lbl));
               else
                  declare
                     C     : constant Expr_Val := Emit_Expr (Ctx, Cond_Exp);
                     CR    : constant String := Emit_Scalar_Coerce (Ctx, To_String (C.Reg), To_String (C.Pir_Type), "i8");
                     L_Ex  : constant String := Next_Label (Ctx.E, "ex_t");
                     L_Cnt : constant String := Next_Label (Ctx.E, "ex_c");
                  begin
                     if Needs_Unwind_To (Ctx, Exit_Depth) then
                        Emit_Term (Ctx.E, "br " & CR & ", " & L_Ex & ", " & L_Cnt);
                        Emit_Label (Ctx.E, L_Ex);
                        Emit_Unwind_To (Ctx, Exit_Depth);
                        Emit_Term (Ctx.E, "jmp " & To_String (Exit_Lbl));
                        Emit_Label (Ctx.E, L_Cnt);
                     else
                        Emit_Term (Ctx.E, "br " & CR & ", " & To_String (Exit_Lbl) & ", " & L_Cnt);
                        Emit_Label (Ctx.E, L_Cnt);
                     end if;
                  end;
               end if;
            end;

         when DN_BLOCK =>
            Emit_Block_Body (Ctx, D (AS_BLOCK_BODY, Stm), Is_Subp => False);

         when DN_RETURN =>
            declare
               Exp : constant TREE := D (AS_EXP, Stm);

               procedure Emit_Copy_Outs is
                  Saved_Handlers : constant Handler_Vecs.Vector := Ctx.Handlers;
               begin
                  Emit_Unwind_To (Ctx, 0, Leave_Masters => True, Release_SS => False, Restore_SP => False);
                  -- LRM 6.4.1 / 11.4.1: copy-out constraint checks propagate to caller, not callee handlers
                  Ctx.Handlers.Clear;
                  for CO of Ctx.Copy_Outs loop
                     if To_String (CO.Pir_Type) = "@__ada_arr_desc" then
                        Emit_Inst (Ctx.E, "memcpy " & To_String (CO.Ptr_Reg) & ", " & To_String (CO.Loc_Reg) & ", " & Img (Arr_Desc_Size) & ", align 8");
                     else
                        declare
                           V : constant String := Next_Reg (Ctx.E, "co");
                        begin
                           Emit_Inst (Ctx.E,
                             V & " = load." & To_String (CO.Pir_Type) & " " &
                             To_String (CO.Loc_Reg) & ", align " & Img (CO.Align));
                           Emit_Range_Check (Ctx, V, To_String (CO.Pir_Type), CO.Ada_Type);
                           Emit_Inst (Ctx.E,
                             "store." & To_String (CO.Pir_Type) & " " & V &
                             ", " & To_String (CO.Ptr_Reg) & ", align " & Img (CO.Align));
                        end;
                     end if;
                  end loop;
                  Ctx.Handlers := Saved_Handlers;
               end Emit_Copy_Outs;
            begin
               if Is_Void_Tree (Exp) then
                  if not Ctx.Accept_Frames.Is_Empty then
                     declare
                        AF : constant Accept_Frame := Ctx.Accept_Frames.Last_Element;
                     begin
                        Emit_Unwind_To (Ctx, AF.Cleanup_Depth);
                        Emit_Term (Ctx.E, "jmp " & To_String (AF.End_Label));
                     end;
                  else
                     Emit_Copy_Outs;
                     Emit_Unwind_To (Ctx, 0, Leave_Masters => False, Release_SS => True, Restore_SP => False);
                     Emit_Term (Ctx.E, "ret");
                  end if;
               elsif Ctx.Cur_Ret_Uncon then
                  -- Unconstrained or dynamic-bound array return -> allocate result on Secondary Stack
                  -- and populate caller's descriptor { ptr data, i32 f1, i32 l1, ... } at %__ret!
                  declare
                     RTI     : constant Type_Info := Get_Type_Info (Ctx, Ctx.Cur_Ret_Type);
                     ETI     : constant Type_Info := Get_Type_Info (Ctx, RTI.Elem_Type);
                     EAl     : constant Natural := Elem_Storage_Align (ETI);
                     ESz     : constant Natural := Elem_Storage_Size (ETI);
                     ND      : constant Natural := Natural'Max (1, Natural'Min ((if RTI.NDims > 0 then RTI.NDims else 1), Max_Array_Dims));
                     Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, ND);
                     Firsts  : Str_Array_4;
                     Lasts   : Str_Array_4;
                     Tot64   : Unbounded_String;
                     SS_Ptr  : Unbounded_String;
                     Unw_Exp : TREE := Exp;
                     Ret_Tmp_Rng_Keys : U32_Vecs.Vector;
                     Ret_Tmp_Exp_Keys : U32_Vecs.Vector;
                  begin
                     -- P3-3.6: Do NOT strip DN_QUALIFIED (so `return S2'(others => 42)` enforces S2's bounds!)
                     while not Is_Void_Tree (Unw_Exp)
                       and then KIND (Unw_Exp) = DN_PARENTHESIZED
                     loop
                        Unw_Exp := D (AS_EXP, Unw_Exp);
                     end loop;

                     -- P3-3.7: For constrained return of named aggregate without `others`, fall into general branch to slide!
                     if KIND (Unw_Exp) = DN_AGGREGATE
                       and then not (RTI.Kind = TK_Constrained_Array and then Is_Named_No_Others_Agg (Unw_Exp, ND))
                     then
                        -- Determine bounds of returned array aggregate (1D positional, named range, or dynamic subtype)
                        if RTI.Kind = TK_Constrained_Array then
                           for Dim in 1 .. ND loop
                              Emit_Index_Bounds (Ctx, RTI.Dims (Dim).Idx_Type, Firsts (Dim), Lasts (Dim));
                           end loop;
                        else
                           -- Unconstrained array aggregate (1D..8D): infer bounds recursively per dimension!
                           declare
                              Cur_Agg : TREE := Unw_Exp;
                           begin
                              for Dim_I in 1 .. ND loop
                                 declare
                                    Assocs : SEQ_TYPE := (if not Is_Void_Tree (Cur_Agg) and then KIND (Cur_Agg) = DN_AGGREGATE
                                                          then LIST (D (AS_GENERAL_ASSOC_S, Cur_Agg)) else MAKE);
                                    Item   : TREE;
                                    Count  : Integer_64 := 0;
                                    Got_R  : Boolean := False;
                                    Next_A : TREE := CONST_VOID;
                                 begin
                                    if not Is_Void_Tree (Cur_Agg) and then KIND (Cur_Agg) = DN_STRING_LITERAL then
                                       Count := Integer_64 (Decode_Ada_String_Lit (Sym_Name (Cur_Agg))'Length);
                                    end if;
                                    while not IS_EMPTY (Assocs) loop
                                       POP (Assocs, Item);
                                       if KIND (Item) = DN_ASSOC or else KIND (Item) = DN_NAMED then
                                          if Is_Void_Tree (Next_A) then
                                             Next_A := D (AS_EXP, Item);
                                          end if;
                                          declare
                                             CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, Item));
                                             Ch : TREE;
                                          begin
                                             while not IS_EMPTY (CS) loop
                                                POP (CS, Ch);
                                                if KIND (Ch) = DN_CHOICE_RANGE then
                                                   declare
                                                      RT, RL, RH : Unbounded_String;
                                                      L64, H64   : Unbounded_String;
                                                      SR : Saved_Range_Info;
                                                      RU : Boolean;
                                                   begin
                                                      Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                                      RU := Dim_I <= RTI.NDims
                                                        and then not Is_Void_Tree (RTI.Dims (Dim_I).Idx_Type)
                                                        and then Is_Unsigned_Scalar (RTI.Dims (Dim_I).Idx_Type, To_String (RT));
                                                      L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64", Is_Unsigned => RU));
                                                      H64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64", Is_Unsigned => RU));
                                                      -- P3-3.8: Cache Pass-1 choice range so Pass 2 does NOT double-evaluate!
                                                      Save_Scalar_Slot (Ctx, To_String (L64), "i64", 8, SR.Lo_Ptr, SR.Is_Global);
                                                      Save_Scalar_Slot (Ctx, To_String (H64), "i64", 8, SR.Hi_Ptr, SR.Is_Global);
                                                      SR.Pir_Type   := To_Unbounded_String ("i64");
                                                      SR.Align      := 8;
                                                      SR.Owner_Subp := Ctx.Cur_Subp_Key;
                                                      Ctx.Saved_Type_Ranges.Include (Tree_Key (Ch), SR);
                                                      Ret_Tmp_Rng_Keys.Append (Tree_Key (Ch));
                                                      if not Got_R then
                                                         Firsts (Dim_I) := L64;
                                                         Lasts (Dim_I)  := H64;
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "rmn");
                                                            ML : constant String := Next_Reg (Ctx.E, "rlo");
                                                            CH : constant String := Next_Reg (Ctx.E, "rmx");
                                                            MH : constant String := Next_Reg (Ctx.E, "rhi");
                                                         begin
                                                            Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i64 " & To_String (L64) & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, ML & " = select.i64 " & CL & ", " & To_String (L64) & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i64 " & To_String (H64) & ", " & To_String (Lasts (Dim_I)));
                                                            Emit_Inst (Ctx.E, MH & " = select.i64 " & CH & ", " & To_String (H64) & ", " & To_String (Lasts (Dim_I)));
                                                            Firsts (Dim_I) := To_Unbounded_String (ML);
                                                            Lasts (Dim_I)  := To_Unbounded_String (MH);
                                                         end;
                                                      end if;
                                                   end;
                                                elsif KIND (Ch) = DN_CHOICE_EXP then
                                                   declare
                                                      ChE  : constant TREE := D (AS_EXP, Ch);
                                                      CE   : constant Expr_Val := Emit_Expr (Ctx, ChE);
                                                      CE_U : constant Boolean := Is_Unsigned_Scalar (CE.Ada_Type, To_String (CE.Pir_Type));
                                                      C64  : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i64", Is_Unsigned => CE_U);
                                                      SE   : Saved_Expr_Info;
                                                   begin
                                                      if CE.Kind = EK_Scalar then
                                                         Save_Scalar_Slot (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), CE.Align, SE.Ptr_Reg, SE.Is_Global);
                                                         SE.Pir_Type   := CE.Pir_Type;
                                                         SE.Ada_Type   := CE.Ada_Type;
                                                         SE.Align      := CE.Align;
                                                         SE.Owner_Subp := Ctx.Cur_Subp_Key;
                                                         Ctx.Saved_Expr_Vals.Include (Tree_Key (ChE), SE);
                                                         Ret_Tmp_Exp_Keys.Append (Tree_Key (ChE));
                                                      end if;
                                                      if not Got_R then
                                                         Firsts (Dim_I) := To_Unbounded_String (C64);
                                                         Lasts (Dim_I)  := To_Unbounded_String (C64);
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "rmn");
                                                            ML : constant String := Next_Reg (Ctx.E, "rlo");
                                                            CH : constant String := Next_Reg (Ctx.E, "rmx");
                                                            MH : constant String := Next_Reg (Ctx.E, "rhi");
                                                         begin
                                                            Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i64 " & C64 & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, ML & " = select.i64 " & CL & ", " & C64 & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i64 " & C64 & ", " & To_String (Lasts (Dim_I)));
                                                            Emit_Inst (Ctx.E, MH & " = select.i64 " & CH & ", " & C64 & ", " & To_String (Lasts (Dim_I)));
                                                            Firsts (Dim_I) := To_Unbounded_String (ML);
                                                            Lasts (Dim_I)  := To_Unbounded_String (MH);
                                                         end;
                                                      end if;
                                                   end;
                                                end if;
                                             end loop;
                                          end;
                                       else
                                          if Is_Void_Tree (Next_A) then
                                             Next_A := Item;
                                          end if;
                                          Count := Count + 1;
                                       end if;
                                    end loop;
                                    if not Got_R then
                                       declare
                                          Idx_Lo_R, Idx_Hi_R : Unbounded_String;
                                          En64 : constant String := Next_Reg (Ctx.E, "re64");
                                       begin
                                          Emit_Index_Bounds (Ctx, RTI.Dims (Dim_I).Idx_Type, Idx_Lo_R, Idx_Hi_R);
                                          Emit_Inst (Ctx.E, En64 & " = add.i64 " & To_String (Idx_Lo_R) & ", " & Img (Long_Long_Integer (Count - 1)));
                                          if Ctx.Enable_Checks and then Count > 0 then
                                             declare
                                                Oob : constant String := Next_Reg (Ctx.E, "raoob");
                                             begin
                                                Emit_Inst (Ctx.E, Oob & " = cmp_gt_s.i64 " & En64 & ", " & To_String (Idx_Hi_R));
                                                Emit_Check_Fail_If (Ctx, Oob);
                                             end;
                                          end if;
                                          Firsts (Dim_I) := Idx_Lo_R;
                                          Lasts (Dim_I)  := To_Unbounded_String (En64);
                                       end;
                                    end if;
                                    Cur_Agg := Next_A;
                                    while not Is_Void_Tree (Cur_Agg)
                                      and then (KIND (Cur_Agg) = DN_PARENTHESIZED or else KIND (Cur_Agg) = DN_QUALIFIED)
                                    loop
                                       Cur_Agg := D (AS_EXP, Cur_Agg);
                                    end loop;
                                 end;
                              end loop;
                           end;
                        end if;

                        declare
                           Byt : Unbounded_String;
                        begin
                           Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND, ESz, Tot64, Byt);
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", To_String (Byt) & ", " & Img (EAl), "ptr", SS_Ptr);
                        end;
                        Emit_Init_Composite (Ctx, To_String (SS_Ptr), Ctx.Cur_Ret_Type, Unw_Exp, Firsts, Lasts);
                        for K_Tmp of Ret_Tmp_Rng_Keys loop
                           Ctx.Saved_Type_Ranges.Exclude (K_Tmp);
                        end loop;
                        for K_Tmp of Ret_Tmp_Exp_Keys loop
                           Ctx.Saved_Expr_Vals.Exclude (K_Tmp);
                        end loop;
                     else
                        -- General array expression: evaluate, check/slide if constrained return type, allocate on SS, copy!
                        declare
                           Eval_RT : constant TREE :=
                             (if KIND (Unw_Exp) = DN_AGGREGATE and then RTI.Kind = TK_Constrained_Array
                              then Subst_Base_Type (Ctx, Ctx.Cur_Ret_Type)
                              else Ctx.Cur_Ret_Type);
                           Src : constant Expr_Val := Emit_Expr (Ctx, Exp, Eval_RT);
                           Byt : Unbounded_String;
                        begin
                           if RTI.Kind = TK_Constrained_Array then
                              declare
                                 Tgt_V : Expr_Val;
                              begin
                                 Tgt_V.Kind     := EK_Slice;
                                 Tgt_V.Ada_Type := Ctx.Cur_Ret_Type;
                                 Tgt_V.NDims    := ND;
                                 for D_Idx in 1 .. ND loop
                                    Emit_Index_Bounds (Ctx, RTI.Dims (D_Idx).Idx_Type, Firsts (D_Idx), Lasts (D_Idx));
                                    Tgt_V.First_Regs (D_Idx) := Firsts (D_Idx);
                                    Tgt_V.Last_Regs (D_Idx)  := Lasts (D_Idx);
                                 end loop;
                                 Emit_Array_Length_Check (Ctx, Tgt_V, Src, ND);
                              end;
                           else
                              for D_Idx in 1 .. ND loop
                                 Get_Array_Dim_Bounds (Ctx, Src, D_Idx, Firsts (D_Idx), Lasts (D_Idx));
                              end loop;
                           end if;
                           Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND, ESz, Tot64, Byt);
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", To_String (Byt) & ", " & Img (EAl), "ptr", SS_Ptr);
                           Emit_Copy_Composite (Ctx, To_String (SS_Ptr), Src, Ctx.Cur_Ret_Type);
                        end;
                     end if;

                     -- Populate caller's descriptor struct at %__ret
                     Emit_Store_Arr_Desc (Ctx, To_String (Ctx.Cur_Ret_Ptr), To_String (SS_Ptr), Firsts, Lasts, ND, Desc_Ty => Desc_Ty);
                     Emit_Copy_Outs;
                     -- Do NOT release SS here: returned buffer lives on caller's Secondary Stack!
                     Emit_Term (Ctx.E, "ret");
                  end;
               elsif Ctx.Cur_Ret_Comp then
                  declare
                     Ret_T  : constant TREE := Ctx.Cur_Ret_Type;
                     Ret_TI : constant Type_Info := Get_Type_Info (Ctx, Ret_T);
                  begin
                     Emit_Init_Composite (Ctx, To_String (Ctx.Cur_Ret_Ptr), Ret_T, Exp);
                     if Ret_TI.Kind = TK_Record then
                        Emit_Discriminant_Check (Ctx, To_String (Ctx.Cur_Ret_Ptr), Ret_T, Is_Access => False);
                     end if;
                  end;
                  Emit_Copy_Outs;
                  Emit_Unwind_To (Ctx, 0, Leave_Masters => False, Release_SS => True, Restore_SP => False);
                  Emit_Term (Ctx.E, "ret");
               else
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, Ctx.Cur_Ret_Type);
                     V   : constant Expr_Val := Emit_Expr (Ctx, Exp, Ctx.Cur_Ret_Type);
                     DP  : constant String := To_String (RTI.Pir_Type);
                     VR  : constant String :=
                       Emit_Typed_Scalar_Convert
                         (Ctx, To_String (V.Reg), V.Ada_Type, To_String (V.Pir_Type),
                          Ctx.Cur_Ret_Type, DP, Check_Range => True);
                  begin
                     Emit_Discriminant_Check (Ctx, VR, Ctx.Cur_Ret_Type, Is_Access => True);
                     Emit_Copy_Outs;
                     Emit_Unwind_To (Ctx, 0, Leave_Masters => False, Release_SS => True, Restore_SP => False);
                     Emit_Term (Ctx.E, "ret " & VR);
                  end;
               end if;
            end;

         when DN_RAISE =>
            declare
               EN : TREE := D (AS_NAME, Stm);
            begin
               if Is_Void_Tree (EN) then
                  -- Bare "raise;" inside exception handler: re-raise current caught exception
                  for I in reverse 0 .. Natural (Ctx.Handlers.Length) - 1 loop
                     if Ctx.Handlers.Element (I).In_Handler then
                        Emit_Raise_Reg (Ctx, To_String (Ctx.Handlers.Element (I).Current_Exn));
                        return;
                     end if;
                  end loop;
                  Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
               else
                  if KIND (EN) = DN_SELECTED then
                     EN := D (AS_DESIGNATOR, EN);
                  end if;
                  Emit_Raise_Tag (Ctx, Get_Exc_Tag (Ctx, D (SM_DEFN, EN)));
               end if;
            end;

         when DN_DELAY =>
            declare
               Ns    : constant String := Emit_Duration_Ns (Ctx, D (AS_EXP, Stm));
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_delay", Ns, "", Dummy, Nounwind => True);
            end;

         when DN_ABORT =>
            -- LRM 9.10(6): evaluate ALL task names first, then abort all (aborting Self last)
            declare
               NS : constant TREE := D (AS_NAME_S, Stm);
            begin
               if not Is_Void_Tree (NS) then
                  declare
                     NL   : SEQ_TYPE := LIST (NS);
                     TN   : TREE;
                     Regs : Str_Vecs.Vector;
                  begin
                     while not IS_EMPTY (NL) loop
                        POP (NL, TN);
                        declare
                           TV : constant Expr_Val := Emit_Expr (Ctx, TN);
                        begin
                           Regs.Append (TV.Reg);
                        end;
                     end loop;
                     if Natural (Regs.Length) = 1 then
                        declare
                           Dummy : Unbounded_String;
                        begin
                           Emit_Call_Or_Invoke (Ctx, "@__ada_task_abort", To_String (Regs.Element (0)), "", Dummy, Nounwind => True);
                        end;
                     elsif not Regs.Is_Empty then
                        declare
                           Cnt   : constant Natural := Natural (Regs.Length);
                           Arr   : constant String := Next_Reg (Ctx.E, "abarr");
                           Dummy : Unbounded_String;
                        begin
                           Emit_Entry_Alloca (Ctx.E, Arr & " = alloca [" & Img (Cnt) & " x ptr], align 8");
                           for I in 0 .. Cnt - 1 loop
                              declare
                                 EP : constant String := Next_Reg (Ctx.E, "abep");
                              begin
                                 Emit_Inst (Ctx.E, EP & " = elem_ptr ptr, " & Arr & ", " & Img (I));
                                 Emit_Inst (Ctx.E, "store.ptr " & To_String (Regs.Element (I)) & ", " & EP & ", align 8");
                              end;
                           end loop;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_task_abort_list", Arr & ", " & Img (Cnt), "", Dummy, Nounwind => True);
                        end;
                     end if;
                  end;
               end if;
            end;

         when DN_TERMINATE =>
            if Length (Ctx.Cur_Task_Exit_Lbl) > 0 then
               -- Unwind any nested block masters/SS/SP inside the task body (task's own master is left at Cur_Task_Exit_Lbl)
               Emit_Unwind_To (Ctx, Ctx.Cur_Task_Base_Cleanup_Depth);
               Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));
            else
               Emit_Term (Ctx.E, "ret");
            end if;

         when DN_PROCEDURE_CALL | DN_ENTRY_CALL | DN_COND_ENTRY | DN_TIMED_ENTRY =>
            declare
               -- P3-4.11: Resolve cross-unit & chained renamed task entries (for normal, conditional, and timed entry calls!)
               function Try_Bind_Cross_Unit_Renamed_Entry (PDef : TREE) return Boolean is
               begin
                  if Is_Void_Tree (PDef) then
                     return False;
                  end if;
                  if Ctx.Renamed_Entries.Contains (Tree_Key (PDef)) then
                     return True;
                  end if;
                  if KIND (PDef) = DN_PROCEDURE_ID
                    and then not Is_Void_Tree (D (SM_UNIT_DESC, PDef))
                    and then KIND (D (SM_UNIT_DESC, PDef)) = DN_RENAMES_UNIT
                  then
                     declare
                        Cur_P    : TREE := PDef;
                        Leaf_Ren : TREE := PDef;
                        Tgt_E    : TREE := CONST_VOID;
                        Tsk_T    : TREE := CONST_VOID;
                     begin
                        for Step in 1 .. 8 loop
                           exit when Is_Void_Tree (Cur_P);
                           if KIND (Cur_P) = DN_ENTRY_ID then
                              Tgt_E := Cur_P;
                              exit;
                           elsif KIND (Cur_P) = DN_PROCEDURE_ID
                             and then not Is_Void_Tree (D (SM_UNIT_DESC, Cur_P))
                             and then KIND (D (SM_UNIT_DESC, Cur_P)) = DN_RENAMES_UNIT
                           then
                              Leaf_Ren := Cur_P;
                              declare
                                 RN : TREE := D (AS_NAME, D (SM_UNIT_DESC, Cur_P));
                              begin
                                 if not Is_Void_Tree (RN) and then KIND (RN) = DN_INDEXED then
                                    RN := D (AS_NAME, RN);
                                 end if;
                                 if not Is_Void_Tree (RN) and then KIND (RN) = DN_SELECTED then
                                    Cur_P := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, RN))));
                                    if not Is_Void_Tree (Cur_P) and then KIND (Cur_P) = DN_ENTRY_ID
                                      and then not Is_Void_Tree (D (AS_NAME, RN))
                                      and then KIND (D (AS_NAME, RN)) in CLASS_EXP
                                      and then KIND (D (AS_NAME, RN)) /= DN_USED_NAME_ID
                                    then
                                       Tsk_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, D (AS_NAME, RN))));
                                    end if;
                                 elsif not Is_Void_Tree (RN) then
                                    Cur_P := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (RN)));
                                 else
                                    exit;
                                 end if;
                              end;
                           else
                              exit;
                           end if;
                        end loop;
                        if not Is_Void_Tree (Tgt_E) then
                           if not Is_Void_Tree (Tsk_T) then
                              if KIND (Tsk_T) = DN_ACCESS then
                                 Register_Task_Spec (Ctx, D (SM_DESIG_TYPE, Tsk_T), Ctx.Cur_Subp_Key);
                              else
                                 Register_Task_Spec (Ctx, Tsk_T, Ctx.Cur_Subp_Key);
                              end if;
                           elsif KIND (Tgt_E) in CLASS_SOURCE_NAME and then not Is_Void_Tree (D (XD_REGION, Tgt_E)) then
                              declare
                                 TR : constant TREE := D (XD_REGION, Tgt_E);
                              begin
                                 if KIND (TR) in CLASS_TYPE_NAME then
                                    Register_Task_Spec (Ctx, D (SM_TYPE_SPEC, TR), Ctx.Cur_Subp_Key);
                                 elsif KIND (TR) = DN_VARIABLE_ID then
                                    Register_Task_Spec (Ctx, D (SM_OBJ_TYPE, TR), Ctx.Cur_Subp_Key);
                                 end if;
                              end;
                           end if;
                           declare
                              RQName : constant String := Qualified_Ada_Name (Leaf_Ren);
                              TSym   : constant String := "@ada_rent_t__" & RQName;
                              ISym   : constant String := "@ada_rent_i__" & RQName;
                           begin
                              Add_Global_Decl (Ctx.E, TSym, "extern " & TSym & " = global ptr, align 8");
                              Add_Global_Decl (Ctx.E, ISym, "extern " & ISym & " = global i32, align 4");
                              Ctx.Renamed_Entries.Include (Tree_Key (PDef),
                                (Task_Sym      => To_Unbounded_String (TSym),
                                 Idx_Sym       => To_Unbounded_String (ISym),
                                 Entry_Id      => Tgt_E,
                                 Is_Local      => False,
                                 Owner_Subp    => 0,
                                 Fallback_TSym => Null_Unbounded_String,
                                 Fallback_ISym => Null_Unbounded_String));
                              return True;
                           end;
                        end if;
                     end;
                  end if;
                  return False;
               end Try_Bind_Cross_Unit_Renamed_Entry;
            begin
               if K = DN_PROCEDURE_CALL then
                  declare
                     PDef : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, Stm))));
                  begin
                     if not Try_Bind_Cross_Unit_Renamed_Entry (PDef) then
                        declare
                           Dummy : constant Expr_Val := Emit_Subp_Call (Ctx, Stm, Is_Func => False);
                        begin
                           pragma Unreferenced (Dummy);
                        end;
                        return;
                     end if;
                  end;
               end if;
            declare
               type Entry_Copy_Back is record
                  Field_Idx : Natural := 0;
                  Act_Ptr   : Unbounded_String;
                  Act_Pir   : Unbounded_String;
                  Act_Align : Natural := 4;
                  Act_Type  : TREE := CONST_VOID;
                  Conv_Type : TREE := CONST_VOID;
                  Pkt_Pir   : Unbounded_String;
                  Pkt_Align : Natural := 4;
                  Pkt_Type  : TREE := CONST_VOID;
                  Is_Shared : Boolean := False;
               end record;
               package ECB_Vecs is new Ada.Containers.Vectors (Natural, Entry_Copy_Back);

               -- Helper: emit entry call (Mode: 0=normal, 1=cond, 2=timed)
               procedure Emit_Entry_Call_Core
                 (EC_Node   : TREE;
                  Mode      : Natural;
                  Delay_Exp : TREE := CONST_VOID;
                  Taken_Out : out Unbounded_String;
                  CB_List   : out ECB_Vecs.Vector;
                  Out_Pkt   : out Unbounded_String;
                  Out_PktT  : out Unbounded_String)
               is
                  Name_N   : TREE := D (AS_NAME, EC_Node);
                  Fam_Exp  : TREE := CONST_VOID;
                  Task_Exp : TREE := CONST_VOID;
                  Entry_Id : TREE := CONST_VOID;
                  Task_Ptr : Unbounded_String;
                  EIdx_Reg : Unbounded_String := To_Unbounded_String ("0");
                  EI       : Entry_Info;
                  Has_EI   : Boolean := False;
                  Pkt_Reg  : Unbounded_String := To_Unbounded_String ("0");
                  Norm_S   : constant TREE := D (SM_NORMALIZED_PARAM_S, EC_Node);
                  Param_L  : SEQ_TYPE := MAKE;
                  Delay_Ns : Unbounded_String := To_Unbounded_String ("0");
               begin
                  Taken_Out := Null_Unbounded_String;
                  CB_List.Clear;
                  Out_Pkt   := Null_Unbounded_String;
                  Out_PktT  := Null_Unbounded_String;
                  if not Is_Void_Tree (Norm_S) then
                     Param_L := LIST (Norm_S);
                  elsif not Is_Void_Tree (D (AS_GENERAL_ASSOC_S, EC_Node)) then
                     Param_L := LIST (D (AS_GENERAL_ASSOC_S, EC_Node));
                  end if;
                  declare
                     Call_Def : constant TREE :=
                       (if KIND (Name_N) /= DN_INDEXED
                        then Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Name_N))) else CONST_VOID);
                     Is_Ren_Ent : constant Boolean := Try_Bind_Cross_Unit_Renamed_Entry (Call_Def);
                  begin
                  if Is_Ren_Ent then
                     declare
                        REI : constant Renamed_Entry_Info := Ctx.Renamed_Entries.Element (Tree_Key (Call_Def));
                        TV  : constant String := Next_Reg (Ctx.E, "rtpv");
                        IV  : constant String := Next_Reg (Ctx.E, "reiv");
                     begin
                        if REI.Is_Local then
                           declare
                              Base_P   : Unbounded_String := REI.Task_Sym;
                              Found_SL : Boolean := (REI.Owner_Subp = Ctx.Cur_Subp_Key);
                              Off_I    : constant String := Next_Reg (Ctx.E, "reio");
                           begin
                              if not Found_SL and then Ctx.Vars.Contains (Tree_Key (Call_Def))
                                and then Ctx.Vars.Element (Tree_Key (Call_Def)).Captured_Slot > 0
                              then
                                 declare
                                    Cap_Sl : constant Natural := Ctx.Vars.Element (Tree_Key (Call_Def)).Captured_Slot;
                                    Cur_SK : Unsigned_32 := Ctx.Cur_Subp_Key;
                                    Cur_SL : Unbounded_String := Ctx.Cur_SL_Reg;
                                 begin
                                    for Step in 1 .. 16 loop
                                       exit when Cur_SK = 0 or else not Ctx.Subps.Contains (Cur_SK);
                                       declare
                                          PSI : constant Subp_Info := Ctx.Subps.Element (Cur_SK);
                                          Par : constant Unsigned_32 := PSI.Parent_Subp;
                                       begin
                                          exit when Par = 0 or else not Ctx.Subps.Contains (Par);
                                          declare
                                             OSI : constant Subp_Info := Ctx.Subps.Element (Par);
                                          begin
                                             if Par = REI.Owner_Subp then
                                                declare
                                                   FP : constant String := Next_Reg (Ctx.E, "rslp");
                                                   LP : constant String := Next_Reg (Ctx.E, "rslv");
                                                begin
                                                   Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (OSI.Frame_Type) & ", " & To_String (Cur_SL) & ", " & Img (Cap_Sl));
                                                   Emit_Inst (Ctx.E, LP & " = load.ptr " & FP & ", align 8");
                                                   Base_P := To_Unbounded_String (LP);
                                                   Found_SL := True;
                                                end;
                                                exit;
                                             else
                                                declare
                                                   FP : constant String := Next_Reg (Ctx.E, "rsln");
                                                   NP : constant String := Next_Reg (Ctx.E, "rslq");
                                                begin
                                                   Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (OSI.Frame_Type) & ", " & To_String (Cur_SL) & ", 0");
                                                   Emit_Inst (Ctx.E, NP & " = load.ptr " & FP & ", align 8");
                                                   Cur_SL := To_Unbounded_String (NP);
                                                   Cur_SK := Par;
                                                end;
                                             end if;
                                          end;
                                       end;
                                    end loop;
                                 end;
                              end if;
                              if Found_SL then
                                 Emit_Inst (Ctx.E, TV & " = load.ptr " & To_String (Base_P) & ", align 8");
                                 declare
                                    Off8 : constant String := Next_Reg (Ctx.E, "rei8");
                                 begin
                                    Emit_Inst (Ctx.E, Off8 & " = const.i64 8");
                                    Emit_Inst (Ctx.E, Off_I & " = ptr_add.i64 " & To_String (Base_P) & ", " & Off8);
                                 end;
                                 Emit_Inst (Ctx.E, IV & " = load.i32 " & Off_I & ", align 4");
                              else
                                 declare
                                    TA : constant String := Next_Reg (Ctx.E, "rtpa");
                                    IA : constant String := Next_Reg (Ctx.E, "reia");
                                 begin
                                    Emit_Inst (Ctx.E, TA & " = addr " & To_String (REI.Fallback_TSym));
                                    Emit_Inst (Ctx.E, TV & " = load.ptr " & TA & ", align 8");
                                    Emit_Inst (Ctx.E, IA & " = addr " & To_String (REI.Fallback_ISym));
                                    Emit_Inst (Ctx.E, IV & " = load.i32 " & IA & ", align 4");
                                 end;
                              end if;
                           end;
                        else
                           declare
                              TA : constant String := Next_Reg (Ctx.E, "rtpa");
                              IA : constant String := Next_Reg (Ctx.E, "reia");
                           begin
                              Emit_Inst (Ctx.E, TA & " = addr " & To_String (REI.Task_Sym));
                              Emit_Inst (Ctx.E, TV & " = load.ptr " & TA & ", align 8");
                              Emit_Inst (Ctx.E, IA & " = addr " & To_String (REI.Idx_Sym));
                              Emit_Inst (Ctx.E, IV & " = load.i32 " & IA & ", align 4");
                           end;
                        end if;
                        Task_Ptr := To_Unbounded_String (TV);
                        EIdx_Reg := To_Unbounded_String (IV);
                        Entry_Id := REI.Entry_Id;
                        if not Is_Void_Tree (Entry_Id) and then Ctx.Entries.Contains (Tree_Key (Entry_Id)) then
                           EI := Ctx.Entries.Element (Tree_Key (Entry_Id));
                           Has_EI := True;
                        end if;
                     end;
                  else
                  if KIND (Name_N) = DN_INDEXED then
                     declare
                        EL : SEQ_TYPE := LIST (D (AS_EXP_S, Name_N));
                     begin
                        if not IS_EMPTY (EL) then POP (EL, Fam_Exp); end if;
                     end;
                     Name_N := D (AS_NAME, Name_N);
                  end if;
                  if KIND (Name_N) = DN_SELECTED then
                     Task_Exp := D (AS_NAME, Name_N);
                     Entry_Id := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, Name_N))));
                  else
                     Entry_Id := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Name_N)));
                  end if;
                  if not Is_Void_Tree (Entry_Id) and then Ctx.Entries.Contains (Tree_Key (Entry_Id)) then
                     EI := Ctx.Entries.Element (Tree_Key (Entry_Id));
                     Has_EI := True;
                  end if;
                  if not Is_Void_Tree (Task_Exp) then
                     declare
                        TV : constant Expr_Val := Emit_Expr (Ctx, Task_Exp);
                        TT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, TV.Ada_Type));
                     begin
                        if Is_Access_Type (TT) then
                           Emit_Null_Check (Ctx, To_String (TV.Reg));
                           declare
                              LP : constant String := Next_Reg (Ctx.E, "atp");
                           begin
                              Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (TV.Reg) & ", align 8");
                              Task_Ptr := To_Unbounded_String (LP);
                           end;
                           if KIND (TT) = DN_ACCESS then
                              Register_Task_Spec (Ctx, D (SM_DESIG_TYPE, TT), Ctx.Cur_Subp_Key);
                           end if;
                        else
                           Task_Ptr := TV.Reg;
                           Register_Task_Spec (Ctx, TT, Ctx.Cur_Subp_Key);
                        end if;
                     end;
                  else
                     Task_Ptr := To_Unbounded_String (Emit_Enclosing_Task_Self_By_Key (Ctx, (if Has_EI then EI.Task_Key else 0)));
                  end if;

                  if not Is_Void_Tree (Entry_Id) and then Ctx.Entries.Contains (Tree_Key (Entry_Id)) then
                     EI := Ctx.Entries.Element (Tree_Key (Entry_Id));
                     Has_EI := True;
                  end if;

                  if Has_EI then
                     EIdx_Reg := To_Unbounded_String (Emit_Entry_Index_Reg (Ctx, EI, Fam_Exp));
                  end if;
                  end if;
                  end;

                  if Has_EI then

                     -- Marshal parameters into stack packet struct (evaluating actuals BEFORE delay!)
                     if EI.Has_Params and then not Is_Void_Tree (EI.Entry_Header)
                       and then not Is_Void_Tree (D (AS_PARAM_S, EI.Entry_Header))
                     then
                        declare
                           PR   : constant String := Next_Reg (Ctx.E, "epkt");
                           FPL  : SEQ_TYPE := LIST (D (AS_PARAM_S, EI.Entry_Header));
                           FP_N : TREE;
                           FIdx : Natural := 0;
                        begin
                           Emit_Entry_Alloca (Ctx.E, PR & " = alloca " & To_String (EI.Pkt_Type) & ", align 8");
                           Pkt_Reg  := To_Unbounded_String (PR);
                           Out_Pkt  := Pkt_Reg;
                           Out_PktT := EI.Pkt_Type;
                           while not IS_EMPTY (FPL) loop
                              POP (FPL, FP_N);
                              declare
                                 PMode : constant NODE_NAME := KIND (FP_N);
                                 NS    : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FP_N));
                                 PID   : TREE;
                              begin
                                 while not IS_EMPTY (NS) loop
                                    POP (NS, PID);
                                    declare
                                       Act : TREE := CONST_VOID;
                                       PT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                                       PTI : constant Type_Info := Get_Type_Info (Ctx, PT);
                                       Fld : constant String := Next_Reg (Ctx.E, "epf");
                                    begin
                                       if not IS_EMPTY (Param_L) then
                                          POP (Param_L, Act);
                                          if KIND (Act) = DN_ASSOC or else KIND (Act) = DN_NAMED then
                                             Act := D (AS_EXP, Act);
                                          end if;
                                       else
                                          Act := D (SM_INIT_EXP, PID);
                                       end if;
                                       if PTI.Kind = TK_Unconstrained_Array
                                         or else (PTI.Kind = TK_Constrained_Array and then PTI.Is_Dynamic)
                                       then
                                          declare
                                             AV : constant Expr_Val := (if PMode = DN_IN then Emit_Expr (Ctx, Act, PT) else Emit_LValue (Ctx, Act));
                                             ND : constant Natural := Natural'Max (1, Natural'Min (PTI.NDims, Max_Array_Dims));
                                             Use_Tgt : Boolean := False;
                                             Tgt_V   : Expr_Val;
                                          begin
                                             if PTI.Kind = TK_Constrained_Array then
                                                declare
                                                   Act_U : TREE := Act;
                                                begin
                                                   while not Is_Void_Tree (Act_U) and then KIND (Act_U) = DN_PARENTHESIZED loop
                                                      Act_U := D (AS_EXP, Act_U);
                                                   end loop;
                                                   Tgt_V.Kind := EK_Slice; Tgt_V.Ada_Type := PT; Tgt_V.NDims := ND;
                                                   for D_I in 1 .. ND loop
                                                      Emit_Index_Bounds (Ctx, PTI.Dims (D_I).Idx_Type, Tgt_V.First_Regs (D_I), Tgt_V.Last_Regs (D_I));
                                                   end loop;
                                                   if not Is_Void_Tree (Act_U)
                                                     and then (KIND (Act_U) = DN_STRING_LITERAL
                                                               or else (KIND (Act_U) = DN_AGGREGATE and then not Is_Named_No_Others_Agg (Act_U, ND)))
                                                   then
                                                      Emit_Array_Length_Check (Ctx, Tgt_V, AV, ND);
                                                   elsif Ctx.Enable_Checks then
                                                      declare
                                                         AV_W : constant Array_View := Resolve_Array_View (Ctx, AV);
                                                      begin
                                                         Emit_Exact_Bounds_Check (Ctx, AV_W.Firsts, AV_W.Lasts, Tgt_V.First_Regs, Tgt_V.Last_Regs, ND);
                                                      end;
                                                   end if;
                                                   Use_Tgt := True;
                                                end;
                                             end if;
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                             Emit_Inst (Ctx.E, "store.ptr " & To_String (AV.Reg) & ", " & Fld & ", align 8");
                                             FIdx := FIdx + 1;
                                             for D_I in 1 .. ND loop
                                                declare
                                                   FR, LR : Unbounded_String;
                                                   FF : constant String := Next_Reg (Ctx.E, "epff");
                                                   LF : constant String := Next_Reg (Ctx.E, "eplf");
                                                begin
                                                   if Use_Tgt then
                                                      FR := Tgt_V.First_Regs (D_I);
                                                      LR := Tgt_V.Last_Regs (D_I);
                                                   else
                                                      Get_Array_Dim_Bounds (Ctx, AV, D_I, FR, LR);
                                                   end if;
                                                   Emit_Inst (Ctx.E, FF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, "store.i64 " & To_String (FR) & ", " & FF & ", align 8");
                                                   FIdx := FIdx + 1;
                                                   Emit_Inst (Ctx.E, LF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, "store.i64 " & To_String (LR) & ", " & LF & ", align 8");
                                                   FIdx := FIdx + 1;
                                                end;
                                             end loop;
                                          end;
                                       elsif PTI.Kind /= TK_Scalar then
                                          declare
                                             AV : constant Expr_Val := (if PMode = DN_IN then Emit_Expr (Ctx, Act, PT) else Emit_LValue (Ctx, Act));
                                          begin
                                             if PTI.Kind = TK_Constrained_Array then
                                                declare
                                                   Tgt_V : Expr_Val;
                                                   ND_A  : constant Natural := Natural'Max (1, Natural'Min (PTI.NDims, Max_Array_Dims));
                                                   Act_U : TREE := Act;
                                                begin
                                                   while not Is_Void_Tree (Act_U) and then KIND (Act_U) = DN_PARENTHESIZED loop
                                                      Act_U := D (AS_EXP, Act_U);
                                                   end loop;
                                                   Tgt_V.Kind := EK_Slice; Tgt_V.Ada_Type := PT; Tgt_V.NDims := ND_A;
                                                   for D_I in 1 .. ND_A loop
                                                      Emit_Index_Bounds (Ctx, PTI.Dims (D_I).Idx_Type, Tgt_V.First_Regs (D_I), Tgt_V.Last_Regs (D_I));
                                                   end loop;
                                                   if not Is_Void_Tree (Act_U) and then KIND (Act_U) in DN_STRING_LITERAL | DN_AGGREGATE then
                                                      Emit_Array_Length_Check (Ctx, Tgt_V, AV, ND_A);
                                                   elsif Ctx.Enable_Checks then
                                                      -- Ada 83 LRM 6.4.1(5) (c95085c, c85018b): exact bounds match for all constrained array entry params
                                                      declare
                                                         AV_W : constant Array_View := Resolve_Array_View (Ctx, AV);
                                                      begin
                                                         Emit_Exact_Bounds_Check (Ctx, AV_W.Firsts, AV_W.Lasts, Tgt_V.First_Regs, Tgt_V.Last_Regs, ND_A);
                                                      end;
                                                   end if;
                                                end;
                                             else
                                                Emit_Discriminant_Check (Ctx, To_String (AV.Reg), PT, Is_Access => False);
                                             end if;
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                             Emit_Inst (Ctx.E, "store.ptr " & To_String (AV.Reg) & ", " & Fld & ", align 8");
                                             FIdx := FIdx + 1;
                                             if Needs_Constrained_Param (Ctx, PT, PMode) then
                                                declare
                                                   CFld : constant String := Next_Reg (Ctx.E, "ecpf");
                                                   CVal : constant String := Emit_Actual_Constrained (Ctx, Act);
                                                begin
                                                   Emit_Inst (Ctx.E, CFld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, "store.i8 " & CVal & ", " & CFld & ", align 1");
                                                   FIdx := FIdx + 1;
                                                end;
                                             end if;
                                          end;
                                       elsif PMode = DN_OUT or else PMode = DN_IN_OUT then
                                          -- Copy-in / copy-out scalar actual (evaluates lvalue ONCE before call, handles DN_CONVERSION!)
                                          declare
                                             Inner_Act : TREE := Act;
                                             Conv_T    : TREE := CONST_VOID;
                                          begin
                                             while not Is_Void_Tree (Inner_Act) and then KIND (Inner_Act) = DN_CONVERSION loop
                                                if Is_Void_Tree (Conv_T) then
                                                   Conv_T := Subst_Tree (Ctx, D (SM_EXP_TYPE, Inner_Act));
                                                end if;
                                                Inner_Act := D (AS_EXP, Inner_Act);
                                             end loop;
                                             declare
                                                LV  : constant Expr_Val := Emit_LValue (Ctx, Inner_Act);
                                                ATI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
                                                Act_Sh : constant Boolean := Is_Shared_Object (Ctx, Inner_Act);
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                if PMode = DN_IN_OUT then
                                                   declare
                                                      RV : constant String := Next_Reg (Ctx.E, "ecin");
                                                      VR : Unbounded_String;
                                                   begin
                                                      if Act_Sh then
                                                         Emit_Inst (Ctx.E, RV & " = load." & To_String (ATI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (ATI.Align) & " [volatile]");
                                                         Emit_Inst (Ctx.E, "fence [seq_cst]");
                                                      else
                                                         Emit_Inst (Ctx.E, RV & " = load." & To_String (ATI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (ATI.Align));
                                                      end if;
                                                      if not Is_Void_Tree (Conv_T) then
                                                         declare
                                                            CTI : constant Type_Info := Get_Type_Info (Ctx, Conv_T);
                                                            CV  : constant String := Emit_Typed_Scalar_Convert (Ctx, RV, LV.Ada_Type, To_String (ATI.Pir_Type), Conv_T, To_String (CTI.Pir_Type));
                                                         begin
                                                            Emit_Range_Check (Ctx, CV, To_String (CTI.Pir_Type), Conv_T);
                                                            VR := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, CV, Conv_T, To_String (CTI.Pir_Type), PT, To_String (PTI.Pir_Type)));
                                                         end;
                                                      else
                                                         VR := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, RV, LV.Ada_Type, To_String (ATI.Pir_Type), PT, To_String (PTI.Pir_Type)));
                                                      end if;
                                                      Emit_Range_Check (Ctx, To_String (VR), To_String (PTI.Pir_Type), PT);
                                                      Emit_Discriminant_Check (Ctx, To_String (VR), PT, Is_Access => True);
                                                      Emit_Inst (Ctx.E, "store." & To_String (PTI.Pir_Type) & " " & To_String (VR) & ", " & Fld & ", align " & Img (PTI.Align));
                                                   end;
                                                end if;
                                                CB_List.Append ((Field_Idx => FIdx,
                                                                 Act_Ptr   => LV.Reg,
                                                                 Act_Pir   => ATI.Pir_Type,
                                                                 Act_Align => ATI.Align,
                                                                 Act_Type  => LV.Ada_Type,
                                                                 Conv_Type => Conv_T,
                                                                 Pkt_Pir   => PTI.Pir_Type,
                                                                 Pkt_Align => PTI.Align,
                                                                 Pkt_Type  => PT,
                                                                 Is_Shared => Act_Sh));
                                                FIdx := FIdx + 1;
                                             end;
                                          end;
                                       else
                                          declare
                                             AV : constant Expr_Val := Emit_Expr (Ctx, Act, PT);
                                             DP : constant String := To_String (PTI.Pir_Type);
                                             VR : constant String :=
                                               Emit_Typed_Scalar_Convert
                                                 (Ctx, To_String (AV.Reg), AV.Ada_Type, To_String (AV.Pir_Type),
                                                  PT, DP, Check_Range => True);
                                          begin
                                             Emit_Discriminant_Check (Ctx, VR, PT, Is_Access => True);
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                             Emit_Inst (Ctx.E, "store." & DP & " " & VR & ", " & Fld & ", align " & Img (PTI.Align));
                                             FIdx := FIdx + 1;
                                          end;
                                       end if;
                                    end;
                                 end loop;
                              end;
                           end loop;
                        end;
                     end if;
                  end if;

                  -- For timed entry calls, evaluate delay expression AFTER entry name & parameters (LRM 9.7.3)
                  if Mode = 2 and then not Is_Void_Tree (Delay_Exp) then
                     Delay_Ns := To_Unbounded_String (Emit_Duration_Ns (Ctx, Delay_Exp));
                  end if;

                  declare
                     RV : Unbounded_String;
                  begin
                     if Mode = 0 then
                        Emit_Call_Or_Invoke (Ctx, "@__ada_entry_call",
                          To_String (Task_Ptr) & ", " & To_String (EIdx_Reg) & ", " & To_String (Pkt_Reg), "", RV);
                     elsif Mode = 1 then
                        Emit_Call_Or_Invoke (Ctx, "@__ada_entry_call_cond",
                          To_String (Task_Ptr) & ", " & To_String (EIdx_Reg) & ", " & To_String (Pkt_Reg), "i8", RV);
                        Taken_Out := RV;
                     else
                        Emit_Call_Or_Invoke (Ctx, "@__ada_entry_call_timed",
                          To_String (Task_Ptr) & ", " & To_String (EIdx_Reg) & ", " & To_String (Pkt_Reg) & ", " & To_String (Delay_Ns), "i8", RV);
                        Taken_Out := RV;
                     end if;
                  end;
               end Emit_Entry_Call_Core;

               procedure Emit_Entry_Copy_Backs
                 (CB_List : ECB_Vecs.Vector;
                  Pkt_Reg : String;
                  Pkt_Ty  : String)
               is
               begin
                  for CB of CB_List loop
                     declare
                        Fld : constant String := Next_Reg (Ctx.E, "ecbf");
                        PV  : constant String := Next_Reg (Ctx.E, "ecbv");
                        CV  : Unbounded_String;
                     begin
                        Emit_Inst (Ctx.E, Fld & " = field_ptr " & Pkt_Ty & ", " & Pkt_Reg & ", " & Img (CB.Field_Idx));
                        Emit_Inst (Ctx.E, PV & " = load." & To_String (CB.Pkt_Pir) & " " & Fld & ", align " & Img (CB.Pkt_Align));
                        if not Is_Void_Tree (CB.Conv_Type) then
                           declare
                              CTI : constant Type_Info := Get_Type_Info (Ctx, CB.Conv_Type);
                              T1  : constant String := Emit_Typed_Scalar_Convert (Ctx, PV, CB.Pkt_Type, To_String (CB.Pkt_Pir), CB.Conv_Type, To_String (CTI.Pir_Type));
                           begin
                              Emit_Range_Check (Ctx, T1, To_String (CTI.Pir_Type), CB.Conv_Type);
                              CV := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, T1, CB.Conv_Type, To_String (CTI.Pir_Type), CB.Act_Type, To_String (CB.Act_Pir)));
                           end;
                        else
                           CV := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, PV, CB.Pkt_Type, To_String (CB.Pkt_Pir), CB.Act_Type, To_String (CB.Act_Pir)));
                        end if;
                        Emit_Range_Check (Ctx, To_String (CV), To_String (CB.Act_Pir), CB.Act_Type);
                        Emit_Discriminant_Check (Ctx, To_String (CV), CB.Act_Type, Is_Access => True);
                        if CB.Is_Shared then
                           Emit_Inst (Ctx.E, "fence [seq_cst]");
                           Emit_Inst (Ctx.E, "store." & To_String (CB.Act_Pir) & " " & To_String (CV) & ", " & To_String (CB.Act_Ptr) & ", align " & Img (CB.Act_Align) & " [volatile]");
                           Emit_Inst (Ctx.E, "fence [seq_cst]");
                        else
                           Emit_Inst (Ctx.E, "store." & To_String (CB.Act_Pir) & " " & To_String (CV) & ", " & To_String (CB.Act_Ptr) & ", align " & Img (CB.Act_Align));
                        end if;
                     end;
                  end loop;
               end Emit_Entry_Copy_Backs;
            begin
               if K = DN_ENTRY_CALL or else K = DN_PROCEDURE_CALL then
                  declare
                     Taken : Unbounded_String;
                     CBL   : ECB_Vecs.Vector;
                     PR, PT : Unbounded_String;
                  begin
                     Emit_Entry_Call_Core (Stm, 0, CONST_VOID, Taken, CBL, PR, PT);
                     Emit_Entry_Copy_Backs (CBL, To_String (PR), To_String (PT));
                  end;
               elsif K = DN_COND_ENTRY then
                  declare
                     L1     : SEQ_TYPE := LIST (D (AS_STM_S1, Stm));
                     EC_S   : TREE := CONST_VOID;
                     L_Tkn  : constant String := Next_Label (Ctx.E, "ce_t");
                     L_Els  : constant String := Next_Label (Ctx.E, "ce_e");
                     L_End  : constant String := Next_Label (Ctx.E, "ce_end");
                     Taken  : Unbounded_String;
                     CBL    : ECB_Vecs.Vector;
                     PR, PT : Unbounded_String;
                  begin
                     while not IS_EMPTY (L1) loop
                        POP (L1, EC_S);
                        exit when KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL;
                     end loop;
                     if not Is_Void_Tree (EC_S) and then (KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL) then
                        Emit_Entry_Call_Core (EC_S, 1, CONST_VOID, Taken, CBL, PR, PT);
                        Emit_Term (Ctx.E, "br " & To_String (Taken) & ", " & L_Tkn & ", " & L_Els);
                        Emit_Label (Ctx.E, L_Tkn);
                        Emit_Entry_Copy_Backs (CBL, To_String (PR), To_String (PT));
                        while not IS_EMPTY (L1) loop
                           declare
                              S2 : TREE;
                           begin
                              POP (L1, S2);
                              if not Is_Terminated (Ctx.E) or else KIND (S2) = DN_LABELED then
                                 Emit_Stm (Ctx, S2);
                              end if;
                           end;
                        end loop;
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_Els);
                        Emit_Stm_List (Ctx, D (AS_STM_S2, Stm));
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_End);
                     end if;
                  end;
               else -- DN_TIMED_ENTRY
                  declare
                     L1     : SEQ_TYPE := LIST (D (AS_STM_S1, Stm));
                     L2     : SEQ_TYPE := LIST (D (AS_STM_S2, Stm));
                     EC_S   : TREE := CONST_VOID;
                     DL_S   : TREE := CONST_VOID;
                     D_Exp  : TREE := CONST_VOID;
                     L_Tkn  : constant String := Next_Label (Ctx.E, "te_t");
                     L_Del  : constant String := Next_Label (Ctx.E, "te_d");
                     L_End  : constant String := Next_Label (Ctx.E, "te_end");
                     Taken  : Unbounded_String;
                     CBL    : ECB_Vecs.Vector;
                     PR, PT : Unbounded_String;
                  begin
                     while not IS_EMPTY (L1) loop
                        POP (L1, EC_S);
                        exit when KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL;
                     end loop;
                     while not IS_EMPTY (L2) loop
                        POP (L2, DL_S);
                        exit when KIND (DL_S) = DN_DELAY;
                     end loop;
                     if not Is_Void_Tree (DL_S) and then KIND (DL_S) = DN_DELAY then
                        D_Exp := D (AS_EXP, DL_S);
                     end if;
                     if not Is_Void_Tree (EC_S) and then (KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL) then
                        Emit_Entry_Call_Core (EC_S, 2, D_Exp, Taken, CBL, PR, PT);
                        Emit_Term (Ctx.E, "br " & To_String (Taken) & ", " & L_Tkn & ", " & L_Del);
                        Emit_Label (Ctx.E, L_Tkn);
                        Emit_Entry_Copy_Backs (CBL, To_String (PR), To_String (PT));
                        while not IS_EMPTY (L1) loop
                           declare
                              S2 : TREE;
                           begin
                              POP (L1, S2);
                              if not Is_Terminated (Ctx.E) or else KIND (S2) = DN_LABELED then
                                 Emit_Stm (Ctx, S2);
                              end if;
                           end;
                        end loop;
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_Del);
                        while not IS_EMPTY (L2) loop
                           declare
                              S2 : TREE;
                           begin
                              POP (L2, S2);
                              if not Is_Terminated (Ctx.E) or else KIND (S2) = DN_LABELED then
                                 Emit_Stm (Ctx, S2);
                              end if;
                           end;
                        end loop;
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_End);
                     end if;
                  end;
               end if;
            end;
            end;

         when DN_ACCEPT | DN_SELECTIVE_WAIT =>
            declare
               procedure Emit_Accept_Core
                 (Acc_Node : TREE;
                  Self_Reg : String;
                  Pkt_Ptr  : String;
                  Call_Reg : String)
               is
                  E_Name : TREE := D (AS_NAME, Acc_Node);
                  E_Def  : TREE;
                  Bound_Ks : U32_Vecs.Vector;
                  Dummy  : Unbounded_String;
                  Acc_PS : constant TREE := D (AS_PARAM_S, Acc_Node);
                  Acc_SS : constant TREE := D (AS_STM_S, Acc_Node);
                  Has_Body : constant Boolean := not Is_Void_Tree (Acc_SS) and then not IS_EMPTY (LIST (Acc_SS));
               begin
                  if KIND (E_Name) = DN_INDEXED then
                     E_Name := D (AS_NAME, E_Name);
                  end if;
                  E_Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (E_Name)));
                  if Has_Body and then not Is_Void_Tree (E_Def) and then Ctx.Entries.Contains (Tree_Key (E_Def)) then
                     declare
                        EI : constant Entry_Info := Ctx.Entries.Element (Tree_Key (E_Def));
                        FIdx : Natural := 0;
                     begin
                        if EI.Has_Params and then not Is_Void_Tree (Acc_PS) then
                           declare
                              PL : SEQ_TYPE := LIST (Acc_PS);
                              P  : TREE;
                           begin
                              while not IS_EMPTY (PL) loop
                                 POP (PL, P);
                                 declare
                                    PMode : constant NODE_NAME := KIND (P);
                                    NS    : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                                    PID   : TREE;
                                 begin
                                    while not IS_EMPTY (NS) loop
                                       POP (NS, PID);
                                       declare
                                          Can_P : constant TREE := Canonical_Def (PID);
                                          PK    : constant Unsigned_32 := Tree_Key (Can_P);
                                          PT    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                                          PTI   : constant Type_Info := Get_Type_Info (Ctx, PT);
                                          Fld   : constant String := Next_Reg (Ctx.E, "apf");
                                          VI    : Var_Info;
                                       begin
                                          VI.Ada_Type   := PT;
                                          VI.Pir_Type   := PTI.Pir_Type;
                                          VI.Align      := PTI.Align;
                                          VI.Owner_Subp := Ctx.Cur_Subp_Key;
                                          if PTI.Kind = TK_Unconstrained_Array
                                            or else (PTI.Kind = TK_Constrained_Array and then PTI.Is_Dynamic)
                                          then
                                             declare
                                                DP : constant String := Next_Reg (Ctx.E, "apd");
                                                ND : constant Natural := Natural'Max (1, Natural'Min (PTI.NDims, Max_Array_Dims));
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                Emit_Inst (Ctx.E, DP & " = load.ptr " & Fld & ", align 8");
                                                FIdx := FIdx + 1;
                                                VI.Kind     := VK_Uncon_Array_Param;
                                                VI.Pir_Name := To_Unbounded_String (DP);
                                                VI.NDims    := ND;
                                                for D_I in 1 .. ND loop
                                                   declare
                                                      FF : constant String := Next_Reg (Ctx.E, "apff");
                                                      FV : constant String := Next_Reg (Ctx.E, "apfv");
                                                      LF : constant String := Next_Reg (Ctx.E, "aplf");
                                                      LV : constant String := Next_Reg (Ctx.E, "aplv");
                                                   begin
                                                      Emit_Inst (Ctx.E, FF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                      Emit_Inst (Ctx.E, FV & " = load.i64 " & FF & ", align 8");
                                                      FIdx := FIdx + 1;
                                                      Emit_Inst (Ctx.E, LF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                      Emit_Inst (Ctx.E, LV & " = load.i64 " & LF & ", align 8");
                                                      FIdx := FIdx + 1;
                                                      VI.First_Regs (D_I) := To_Unbounded_String (FV);
                                                      VI.Last_Regs (D_I)  := To_Unbounded_String (LV);
                                                   end;
                                                end loop;
                                             end;
                                          elsif PTI.Kind /= TK_Scalar then
                                             declare
                                                PV : constant String := Next_Reg (Ctx.E, "app");
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                Emit_Inst (Ctx.E, PV & " = load.ptr " & Fld & ", align 8");
                                                FIdx := FIdx + 1;
                                                VI.Kind     := VK_Param_Ptr;
                                                VI.Pir_Name := To_Unbounded_String (PV);
                                                if Needs_Constrained_Param (Ctx, PT, PMode) then
                                                   declare
                                                      CFld : constant String := Next_Reg (Ctx.E, "apcf");
                                                      CVal : constant String := Next_Reg (Ctx.E, "apcv");
                                                   begin
                                                      Emit_Inst (Ctx.E, CFld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                      Emit_Inst (Ctx.E, CVal & " = load.i8 " & CFld & ", align 1");
                                                      FIdx := FIdx + 1;
                                                      VI.Constrained_Reg := To_Unbounded_String (CVal);
                                                   end;
                                                end if;
                                             end;
                                          elsif PMode = DN_OUT or else PMode = DN_IN_OUT then
                                             -- Copy-in/copy-out scalar param: formal points directly into packet slot!
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                             FIdx := FIdx + 1;
                                             VI.Kind     := VK_Param_Ptr;
                                             VI.Pir_Name := To_Unbounded_String (Fld);
                                          else
                                             declare
                                                SV : constant String := Next_Reg (Ctx.E, "apv");
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                Emit_Inst (Ctx.E, SV & " = load." & To_String (PTI.Pir_Type) & " " & Fld & ", align " & Img (PTI.Align));
                                                FIdx := FIdx + 1;
                                                VI.Kind     := VK_Param_Val;
                                                VI.Pir_Name := To_Unbounded_String (SV);
                                             end;
                                          end if;
                                          if Ctx.Vars.Contains (PK) and then Ctx.Vars.Element (PK).Owner_Subp = Ctx.Cur_Subp_Key then
                                             VI.Captured_Slot := Ctx.Vars.Element (PK).Captured_Slot;
                                          end if;
                                          if VI.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                                             declare
                                                FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                                FPR : constant String := Next_Reg (Ctx.E, "afp");
                                             begin
                                                if VI.Kind = VK_Param_Val then
                                                   declare
                                                      Sp : constant String := Next_Reg (Ctx.E, "asp");
                                                   begin
                                                      Emit_Entry_Alloca (Ctx.E, Sp & " = alloca " & To_String (PTI.Pir_Type) & ", align " & Img (Natural'Max (1, PTI.Align)));
                                                      Emit_Inst (Ctx.E, "store." & To_String (PTI.Pir_Type) & " " & To_String (VI.Pir_Name) & ", " & Sp & ", align " & Img (Natural'Max (1, PTI.Align)));
                                                      Emit_Inst (Ctx.E, FPR & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VI.Captured_Slot));
                                                      Emit_Inst (Ctx.E, "store.ptr " & Sp & ", " & FPR & ", align 8");
                                                      VI.Kind     := VK_Local_Alloca;
                                                      VI.Pir_Name := To_Unbounded_String (Sp);
                                                   end;
                                                elsif VI.Kind = VK_Param_Ptr then
                                                   Emit_Inst (Ctx.E, FPR & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VI.Captured_Slot));
                                                   Emit_Inst (Ctx.E, "store.ptr " & To_String (VI.Pir_Name) & ", " & FPR & ", align 8");
                                                elsif VI.Kind = VK_Uncon_Array_Param then
                                                   declare
                                                      DR : constant String := Next_Reg (Ctx.E, "apdr");
                                                   begin
                                                      Emit_Entry_Alloca (Ctx.E, DR & " = alloca @__ada_arr_desc, align 8");
                                                      Emit_Store_Arr_Desc (Ctx, DR, To_String (VI.Pir_Name), VI.First_Regs, VI.Last_Regs, VI.NDims);
                                                      Emit_Inst (Ctx.E, FPR & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VI.Captured_Slot));
                                                      Emit_Inst (Ctx.E, "store.ptr " & DR & ", " & FPR & ", align 8");
                                                      VI.Has_Desc := True;
                                                   end;
                                                end if;
                                             end;
                                          end if;
                                          Ctx.Vars.Include (PK, VI);
                                          Ctx.Vars.Include (Tree_Key (PID), VI);
                                          Bound_Ks.Append (PK);
                                       end;
                                    end loop;
                                 end;
                              end loop;
                           end;
                        end if;
                     end;
                  end if;

                  if Has_Body then
                     -- Wrap accept body so unhandled exception propagates to BOTH caller and acceptor (LRM 11.5)
                     declare
                        H_Lbl  : constant String := Next_Label (Ctx.E, "acc_exc");
                        R_Lbl  : constant String := Next_Label (Ctx.E, "acc_ret");
                        E_Lbl  : constant String := Next_Label (Ctx.E, "acc_end");
                        Exn_R  : constant String := Next_Reg (Ctx.E, "aex");
                        H_Mark   : constant Unbounded_String := Latest_Cleanup_Reg (Ctx, CK_SS_Mark);
                        H_SPMark : constant Unbounded_String := Latest_Cleanup_Reg (Ctx, CK_SP_Mark);
                     begin
                        Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (H_Lbl),
                                              Current_Exn   => Null_Unbounded_String,
                                              In_Handler    => False,
                                              SS_Mark       => H_Mark,
                                              SP_Mark       => H_SPMark));
                        Ctx.Accept_Frames.Append ((End_Label     => To_Unbounded_String (R_Lbl),
                                                   Cleanup_Depth => Cleanup_Depth (Ctx)));
                        Emit_Stm_List (Ctx, Acc_SS);
                        Ctx.Accept_Frames.Delete_Last;
                        Ctx.Handlers.Delete_Last;
                        if not Is_Terminated (Ctx.E) then
                           Emit_Term (Ctx.E, "jmp " & R_Lbl);
                        end if;
                        Emit_Label (Ctx.E, R_Lbl);
                        Emit_Call_Or_Invoke (Ctx, "@__ada_accept_end", Self_Reg & ", " & Call_Reg & ", 0", "", Dummy, Nounwind => True);
                        Emit_Term (Ctx.E, "jmp " & E_Lbl);
                        Emit_Label (Ctx.E, H_Lbl, Exn_R & ": ptr");
                        Emit_Call_Or_Invoke (Ctx, "@__ada_accept_end", Self_Reg & ", " & Call_Reg & ", " & Exn_R, "", Dummy, Nounwind => True);
                        Emit_Raise_Reg (Ctx, Exn_R);
                        Emit_Label (Ctx.E, E_Lbl);
                     end;
                  else
                     Emit_Call_Or_Invoke (Ctx, "@__ada_accept_end", Self_Reg & ", " & Call_Reg & ", 0", "", Dummy, Nounwind => True);
                  end if;

                  for BK of Bound_Ks loop
                     if Ctx.Vars.Contains (BK) then
                        declare
                           Old_V : Var_Info := Ctx.Vars.Element (BK);
                        begin
                           if Old_V.Captured_Slot > 0 and then Old_V.Owner_Subp = Ctx.Cur_Subp_Key then
                              Old_V.Pir_Name := Null_Unbounded_String;
                              Ctx.Vars.Replace (BK, Old_V);
                           else
                              Ctx.Vars.Delete (BK);
                           end if;
                        end;
                     end if;
                  end loop;
               end Emit_Accept_Core;

               function Compute_Accept_EIdx (Acc_Node : TREE) return String is
                  E_Name : TREE := D (AS_NAME, Acc_Node);
                  Fam_E  : TREE := CONST_VOID;
                  E_Def  : TREE;
               begin
                  if KIND (E_Name) = DN_INDEXED then
                     declare
                        EL : SEQ_TYPE := LIST (D (AS_EXP_S, E_Name));
                     begin
                        if not IS_EMPTY (EL) then POP (EL, Fam_E); end if;
                     end;
                     E_Name := D (AS_NAME, E_Name);
                  end if;
                  E_Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (E_Name)));
                  if not Is_Void_Tree (E_Def) and then Ctx.Entries.Contains (Tree_Key (E_Def)) then
                     declare
                        EI : constant Entry_Info := Ctx.Entries.Element (Tree_Key (E_Def));
                     begin
                        return Emit_Entry_Index_Reg (Ctx, EI, Fam_E);
                     end;
                  end if;
                  declare
                     C : constant String := Next_Reg (Ctx.E, "aez");
                  begin
                     Emit_Inst (Ctx.E, C & " = const.i32 0");
                     return C;
                  end;
               end Compute_Accept_EIdx;

               Self_R : Unbounded_String := Ctx.Cur_Task_Reg;
            begin
               if Length (Self_R) = 0 then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_task_self", "", "ptr", Self_R, Nounwind => True);
               end if;
               if K = DN_ACCEPT then
                  declare
                     EIdx     : constant String := Compute_Accept_EIdx (Stm);
                     Pkt_Slot : constant String := Next_Reg (Ctx.E, "apks");
                     Call_R   : Unbounded_String;
                     Pkt_R    : constant String := Next_Reg (Ctx.E, "apkr");
                  begin
                     Emit_Entry_Alloca (Ctx.E, Pkt_Slot & " = alloca ptr, align 8");
                     Emit_Call_Or_Invoke (Ctx, "@__ada_accept_begin",
                       To_String (Self_R) & ", " & EIdx & ", " & Pkt_Slot, "ptr", Call_R);
                     Emit_Inst (Ctx.E, Pkt_R & " = load.ptr " & Pkt_Slot & ", align 8");
                     Emit_Accept_Core (Stm, To_String (Self_R), Pkt_R, To_String (Call_R));
                  end;
               else -- DN_SELECTIVE_WAIT
                  declare
                     Clauses : SEQ_TYPE := LIST (D (AS_TEST_CLAUSE_ELEM_S, Stm));
                     C_Node  : TREE;
                     type Sel_Arm is record
                        Alt_Kind : Natural := 0; -- 1=accept, 2=delay, 3=terminate
                        Guard    : TREE := CONST_VOID;
                        First_S  : TREE := CONST_VOID;
                        Stm_S    : TREE := CONST_VOID;
                        Arm_Lbl  : Unbounded_String;
                     end record;
                     package Sel_Arm_Vecs is new Ada.Containers.Vectors (Natural, Sel_Arm);
                     Arms     : Sel_Arm_Vecs.Vector;
                     NArm     : Natural := 0;
                     Else_SS  : constant TREE := D (AS_STM_S, Stm);
                     Has_Else : constant Boolean := not Is_Void_Tree (Else_SS) and then not IS_EMPTY (LIST (Else_SS));
                  begin
                     while not IS_EMPTY (Clauses) loop
                        POP (Clauses, C_Node);
                        if KIND (C_Node) = DN_SELECT_ALTERNATIVE then
                           declare
                              SS : constant TREE := D (AS_STM_S, C_Node);
                              SL : SEQ_TYPE := (if Is_Void_Tree (SS) then MAKE else LIST (SS));
                              FS : TREE := CONST_VOID;
                              A  : Sel_Arm;
                           begin
                              while not IS_EMPTY (SL) loop
                                 POP (SL, FS);
                                 exit when KIND (FS) /= DN_STM_PRAGMA;
                              end loop;
                              A.Guard   := D (AS_EXP, C_Node);
                              A.First_S := FS;
                              A.Stm_S   := SS;
                              A.Arm_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "sw_a"));
                              if not Is_Void_Tree (FS) then
                                 if KIND (FS) = DN_ACCEPT then A.Alt_Kind := 1;
                                 elsif KIND (FS) = DN_DELAY then A.Alt_Kind := 2;
                                 elsif KIND (FS) = DN_TERMINATE then A.Alt_Kind := 3;
                                 end if;
                              end if;
                              Arms.Append (A);
                              NArm := NArm + 1;
                           end;
                        end if;
                     end loop;

                     declare
                        Alloc_N  : constant Natural := Natural'Max (1, NArm);
                        AK_Arr   : constant String := Next_Reg (Ctx.E, "swk");
                        EI_Arr   : constant String := Next_Reg (Ctx.E, "swe");
                        DL_Arr   : constant String := Next_Reg (Ctx.E, "swd");
                        Pkt_Slot : constant String := Next_Reg (Ctx.E, "swps");
                        Cal_Slot : constant String := Next_Reg (Ctx.E, "swcs");
                        Chosen   : Unbounded_String;
                        L_End    : constant String := Next_Label (Ctx.E, "sw_end");
                        L_Else   : constant String := Next_Label (Ctx.E, "sw_els");
                        L_Term   : constant String := Next_Label (Ctx.E, "sw_trm");
                        Sw_Str   : Unbounded_String;
                     begin
                        Emit_Entry_Alloca (Ctx.E, AK_Arr & " = alloca [" & Img (Alloc_N) & " x i32], align 4");
                        Emit_Entry_Alloca (Ctx.E, EI_Arr & " = alloca [" & Img (Alloc_N) & " x i32], align 4");
                        Emit_Entry_Alloca (Ctx.E, DL_Arr & " = alloca [" & Img (Alloc_N) & " x i64], align 8");
                        Emit_Entry_Alloca (Ctx.E, Pkt_Slot & " = alloca ptr, align 8");
                        Emit_Entry_Alloca (Ctx.E, Cal_Slot & " = alloca ptr, align 8");

                        -- P3-4.15: LRM 9.7.1(5) Pass 1: evaluate ALL guards before any delay/entry-family index expressions
                        for I in 0 .. NArm - 1 loop
                           declare
                              KP : constant String := Next_Reg (Ctx.E, "swkp");
                              EP : constant String := Next_Reg (Ctx.E, "swep");
                              DP : constant String := Next_Reg (Ctx.E, "swdp");
                           begin
                              Emit_Inst (Ctx.E, KP & " = elem_ptr i32, " & AK_Arr & ", " & Img (I));
                              Emit_Inst (Ctx.E, EP & " = elem_ptr i32, " & EI_Arr & ", " & Img (I));
                              Emit_Inst (Ctx.E, DP & " = elem_ptr i64, " & DL_Arr & ", " & Img (I));
                              Emit_Inst (Ctx.E, "store.i32 0, " & EP & ", align 4");
                              Emit_Inst (Ctx.E, "store.i64 0, " & DP & ", align 8");
                              if not Is_Void_Tree (Arms (I).Guard) then
                                 declare
                                    GV  : constant Expr_Val := Emit_Expr (Ctx, Arms (I).Guard);
                                    Sel : constant String := Next_Reg (Ctx.E, "swks");
                                 begin
                                    Emit_Inst (Ctx.E, Sel & " = select.i32 " & To_String (GV.Reg) & ", " & Img (Arms (I).Alt_Kind) & ", 0");
                                    Emit_Inst (Ctx.E, "store.i32 " & Sel & ", " & KP & ", align 4");
                                 end;
                              else
                                 Emit_Inst (Ctx.E, "store.i32 " & Img (Arms (I).Alt_Kind) & ", " & KP & ", align 4");
                              end if;
                           end;
                        end loop;

                        -- Pass 2: evaluate entry-family indices and delay durations ONLY for open alternatives
                        for I in 0 .. NArm - 1 loop
                           if Arms (I).Alt_Kind = 1 or else Arms (I).Alt_Kind = 2 then
                              declare
                                 KP : constant String := Next_Reg (Ctx.E, "swk2");
                                 EP : constant String := Next_Reg (Ctx.E, "swe2");
                                 DP : constant String := Next_Reg (Ctx.E, "swd2");
                              begin
                                 Emit_Inst (Ctx.E, KP & " = elem_ptr i32, " & AK_Arr & ", " & Img (I));
                                 Emit_Inst (Ctx.E, EP & " = elem_ptr i32, " & EI_Arr & ", " & Img (I));
                                 Emit_Inst (Ctx.E, DP & " = elem_ptr i64, " & DL_Arr & ", " & Img (I));
                                 if not Is_Void_Tree (Arms (I).Guard) then
                                    declare
                                       KV    : constant String := Next_Reg (Ctx.E, "swkv");
                                       Is_Op : constant String := Next_Reg (Ctx.E, "swop");
                                       L_Opn : constant String := Next_Label (Ctx.E, "swg_o");
                                       L_Cls : constant String := Next_Label (Ctx.E, "swg_c");
                                    begin
                                       Emit_Inst (Ctx.E, KV & " = load.i32 " & KP & ", align 4");
                                       Emit_Inst (Ctx.E, Is_Op & " = cmp_ne.i32 " & KV & ", 0");
                                       Emit_Term (Ctx.E, "br " & Is_Op & ", " & L_Opn & ", " & L_Cls);
                                       Emit_Label (Ctx.E, L_Opn);
                                       if Arms (I).Alt_Kind = 1 then
                                          Emit_Inst (Ctx.E, "store.i32 " & Compute_Accept_EIdx (Arms (I).First_S) & ", " & EP & ", align 4");
                                       else
                                          Emit_Inst (Ctx.E, "store.i64 " & Emit_Duration_Ns (Ctx, D (AS_EXP, Arms (I).First_S)) & ", " & DP & ", align 8");
                                       end if;
                                       Emit_Term (Ctx.E, "jmp " & L_Cls);
                                       Emit_Label (Ctx.E, L_Cls);
                                    end;
                                 else
                                    if Arms (I).Alt_Kind = 1 then
                                       Emit_Inst (Ctx.E, "store.i32 " & Compute_Accept_EIdx (Arms (I).First_S) & ", " & EP & ", align 4");
                                    else
                                       Emit_Inst (Ctx.E, "store.i64 " & Emit_Duration_Ns (Ctx, D (AS_EXP, Arms (I).First_S)) & ", " & DP & ", align 8");
                                    end if;
                                 end if;
                              end;
                           end if;
                        end loop;

                        Emit_Call_Or_Invoke (Ctx, "@__ada_select_wait",
                          To_String (Self_R) & ", " & Img (NArm) & ", " &
                          AK_Arr & ", " & EI_Arr & ", " & DL_Arr & ", " &
                          (if Has_Else then "1" else "0") & ", " & Pkt_Slot & ", " & Cal_Slot,
                          "i32", Chosen);

                        Append (Sw_Str, "-2 -> " & L_Term);
                        if Has_Else then
                           Append (Sw_Str, ", -1 -> " & L_Else);
                        end if;
                        for I in 0 .. NArm - 1 loop
                           Append (Sw_Str, ", " & Img (I) & " -> " & To_String (Arms (I).Arm_Lbl));
                        end loop;
                        Emit_Term (Ctx.E, "switch.i32 " & To_String (Chosen) & ", default " & L_End & " [ " & To_String (Sw_Str) & " ]");

                        Emit_Label (Ctx.E, L_Term);
                        if Length (Ctx.Cur_Task_Exit_Lbl) > 0 then
                           Emit_Unwind_To (Ctx, Ctx.Cur_Task_Base_Cleanup_Depth);
                           Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));
                        else
                           Emit_Term (Ctx.E, "ret");
                        end if;

                        if Has_Else then
                           Emit_Label (Ctx.E, L_Else);
                           Emit_Stm_List (Ctx, Else_SS);
                           if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        end if;

                        for I in 0 .. NArm - 1 loop
                           Emit_Label (Ctx.E, To_String (Arms (I).Arm_Lbl));
                           declare
                              SL : SEQ_TYPE := (if Is_Void_Tree (Arms (I).Stm_S) then MAKE else LIST (Arms (I).Stm_S));
                              S_It : TREE;
                              Past_First : Boolean := False;
                           begin
                              while not IS_EMPTY (SL) loop
                                 POP (SL, S_It);
                                 if not Past_First then
                                    if S_It = Arms (I).First_S then
                                       Past_First := True;
                                       if Arms (I).Alt_Kind = 1 then
                                          declare
                                             PR : constant String := Next_Reg (Ctx.E, "swpr");
                                             CR : constant String := Next_Reg (Ctx.E, "swcr");
                                          begin
                                             Emit_Inst (Ctx.E, PR & " = load.ptr " & Pkt_Slot & ", align 8");
                                             Emit_Inst (Ctx.E, CR & " = load.ptr " & Cal_Slot & ", align 8");
                                             Emit_Accept_Core (Arms (I).First_S, To_String (Self_R), PR, CR);
                                          end;
                                       end if;
                                    end if;
                                 else
                                    if not Is_Terminated (Ctx.E) or else KIND (S_It) = DN_LABELED then
                                       Emit_Stm (Ctx, S_It);
                                    end if;
                                 end if;
                              end loop;
                           end;
                           if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        end loop;
                        Emit_Label (Ctx.E, L_End);
                     end;
                  end;
               end if;
            end;

         when others =>
            Emit_Comment (Ctx.E, "Unhandled statement: " & NODE_IMAGE (K));
      end case;
   end Emit_Stm;

end Cg_Stmt;
