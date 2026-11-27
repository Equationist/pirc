-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;

package body Cg_Expr.Composite is

   procedure Emit_Copy_Composite
     (Ctx      : in out Context;
      Dst_Ptr  : String;
      Src      : Expr_Val;
      Dst_Type : TREE)
   is
      DTI : constant Type_Info := Get_Type_Info (Ctx, Dst_Type);
   begin
      if DTI.Kind = TK_Record or else (DTI.Kind = TK_Constrained_Array and then not DTI.Is_Dynamic) then
         if DTI.Byte_Size > 0 then
            Emit_Inst (Ctx.E,
              "memmove " & Dst_Ptr & ", " & To_String (Src.Reg) & ", " &
              Img (DTI.Byte_Size) & ", align " & Img (DTI.Align));
         end if;
         -- Recursively deep-copy any embedded dynamic constrained array fields (@__ada_arr_desc)
         -- in records, nested records, active variants, and arrays of records (c52008b)
         declare
            function Type_Has_Dyn_Arr (T : TREE; Depth : Natural := 0) return Boolean is
               TI : Type_Info;
               BT : TREE;
               function CL_Has_Dyn (CL : TREE) return Boolean is
               begin
                  if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then
                     return False;
                  end if;
                  if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                     declare
                        DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                        DN : TREE;
                     begin
                        while not IS_EMPTY (DL) loop
                           POP (DL, DN);
                           if KIND (DN) = DN_VARIABLE_DECL then
                              declare
                                 NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DN));
                                 CID : TREE;
                              begin
                                 while not IS_EMPTY (NS) loop
                                    POP (NS, CID);
                                    if Type_Has_Dyn_Arr (Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID)), Depth + 1) then
                                       return True;
                                    end if;
                                 end loop;
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
                              if KIND (VN) = DN_VARIANT and then CL_Has_Dyn (D (AS_COMP_LIST, VN)) then
                                 return True;
                              end if;
                           end loop;
                        end;
                     end if;
                  end;
                  return False;
               end CL_Has_Dyn;
            begin
               if Depth > 16 or else Is_Void_Tree (T) then
                  return False;
               end if;
               TI := Get_Type_Info (Ctx, T);
               if TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic then
                  return True;
               elsif TI.Kind = TK_Constrained_Array then
                  return Type_Has_Dyn_Arr (TI.Elem_Type, Depth + 1);
               elsif TI.Kind = TK_Record then
                  BT := Base_Type_Spec (Subst_Tree (Ctx, T));
                  if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
                     return CL_Has_Dyn (D (SM_COMP_LIST, BT));
                  end if;
               end if;
               return False;
            end Type_Has_Dyn_Arr;

            procedure Deep_Copy_Fields (Cur_Ptr : String; Cur_T : TREE) is
               CTI_Top : constant Type_Info := Get_Type_Info (Ctx, Cur_T);

               procedure Clone_Dyn_Arr (Arr_Desc_Ptr : String; Arr_T : TREE; ATI : Type_Info) is
                  ETI   : constant Type_Info := Get_Type_Info (Ctx, ATI.Elem_Type);
                  Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
                  EAl   : constant Natural := Elem_Storage_Align (ETI);
                  ESz   : constant Natural := Elem_Storage_Size (ETI);
                  ND_A  : constant Natural := Natural'Max (1, Natural'Min (ATI.NDims, Max_Array_Dims));
                  Orig  : Expr_Val;
                  Tot64 : Unbounded_String;
                  Byt   : Unbounded_String;
                  Nz    : constant String := Next_Reg (Ctx.E, "dcnz");
                  AlB   : constant String := Next_Reg (Ctx.E, "dcal");
                  NewP  : Unbounded_String;
                  P0    : constant String := Next_Reg (Ctx.E, "dcp0");
               begin
                  Emit_Unpack_Arr_Desc (Ctx, Arr_Desc_Ptr, Arr_T, Orig);
                  Emit_Array_Byte_Size_I64 (Ctx, Orig.First_Regs, Orig.Last_Regs, ND_A, ESz, Tot64, Byt);
                  Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                  Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", NewP);
                  Emit_Inst (Ctx.E, "memcpy " & To_String (NewP) & ", " & To_String (Orig.Reg) & ", " & To_String (Byt) & ", align " & Img (EAl));
                  Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & Arr_Desc_Ptr & ", 0");
                  Emit_Inst (Ctx.E, "store.ptr " & To_String (NewP) & ", " & P0 & ", align 8");
                  if Is_DE or else Type_Has_Dyn_Arr (ATI.Elem_Type) then
                     declare
                        Zero64 : constant String := Next_Reg (Ctx.E, "dca0");
                        L_H    : constant String := Next_Label (Ctx.E, "dcah");
                        L_B    : constant String := Next_Label (Ctx.E, "dcab");
                        L_E    : constant String := Next_Label (Ctx.E, "dcae");
                        IR     : constant String := Next_Reg (Ctx.E, "dcai");
                        Cmp_R  : constant String := Next_Reg (Ctx.E, "dcac");
                        Off_R  : constant String := Next_Reg (Ctx.E, "dcao");
                        EP_R   : constant String := Next_Reg (Ctx.E, "dcap");
                        Nxt_R  : constant String := Next_Reg (Ctx.E, "dcan");
                     begin
                        Emit_Inst (Ctx.E, Zero64 & " = const.i64 0");
                        Emit_Term (Ctx.E, "jmp " & L_H & "(" & Zero64 & ")");
                        Emit_Label (Ctx.E, L_H, IR & ": i64");
                        Emit_Inst (Ctx.E, Cmp_R & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                        Emit_Term (Ctx.E, "br " & Cmp_R & ", " & L_B & ", " & L_E);
                        Emit_Label (Ctx.E, L_B);
                        Emit_Inst (Ctx.E, Off_R & " = mul.i64 " & IR & ", " & Img (ESz));
                        Emit_Inst (Ctx.E, EP_R & " = ptr_add.i64 " & To_String (NewP) & ", " & Off_R & " [inbounds]");
                        if Is_DE then
                           Clone_Dyn_Arr (EP_R, ATI.Elem_Type, ETI);
                        else
                           Deep_Copy_Fields (EP_R, ATI.Elem_Type);
                        end if;
                        Emit_Inst (Ctx.E, Nxt_R & " = add.i64 " & IR & ", 1");
                        Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nxt_R & ")");
                        Emit_Label (Ctx.E, L_E);
                     end;
                  end if;
               end Clone_Dyn_Arr;

               procedure Walk_Rec_CL (Rec_P : String; Rec_Pir : String; CL : TREE) is
               begin
                  if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then
                     return;
                  end if;
                  if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                     declare
                        DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                        DN : TREE;
                     begin
                        while not IS_EMPTY (DL) loop
                           POP (DL, DN);
                           if KIND (DN) = DN_VARIABLE_DECL then
                              declare
                                 NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DN));
                                 CID : TREE;
                              begin
                                 while not IS_EMPTY (NS) loop
                                    POP (NS, CID);
                                    declare
                                       CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                                       CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                       CK  : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                    begin
                                       if Ctx.Fields.Contains (CK) and then Type_Has_Dyn_Arr (CT) then
                                          declare
                                             FE : constant Field_Entry := Ctx.Fields.Element (CK);
                                             FP : constant String := Emit_Field_Addr (Ctx, Rec_P, Rec_Pir, FE, "dcfp");
                                          begin
                                             if CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                                                Clone_Dyn_Arr (FP, CT, CTI);
                                             else
                                                Deep_Copy_Fields (FP, CT);
                                             end if;
                                          end;
                                       end if;
                                    end;
                                 end loop;
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
                           D_Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, VP))));
                           DK    : constant Unsigned_32 := Tree_Key (D_Def);
                        begin
                           if Ctx.Fields.Contains (DK) then
                              declare
                                 DFE   : constant Field_Entry := Ctx.Fields.Element (DK);
                                 DT_A  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, D_Def));
                                 DTI_D : constant Type_Info := Get_Type_Info (Ctx, DT_A);
                                 D64   : constant String := Emit_Load_Field_I64 (Ctx, Rec_P, Rec_Pir, DFE, DT_A, DTI_D);
                                 VL    : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                                 VN    : TREE;
                                 L_End : constant String := Next_Label (Ctx.E, "dcve");
                              begin
                                 while not IS_EMPTY (VL) loop
                                    POP (VL, VN);
                                    if KIND (VN) = DN_VARIANT then
                                       declare
                                          Is_Ot : Boolean := False;
                                          Cond  : constant String :=
                                            Emit_Variant_Choice_Match (Ctx, D64, DT_A, D (AS_CHOICE_S, VN), Is_Ot);
                                          L_Hit : constant String := Next_Label (Ctx.E, "dcvh");
                                          L_Nxt : constant String := Next_Label (Ctx.E, "dcvn");
                                       begin
                                          if Is_Ot then
                                             Emit_Term (Ctx.E, "jmp " & L_Hit);
                                          elsif Cond'Length > 0 then
                                             Emit_Term (Ctx.E, "br " & Cond & ", " & L_Hit & ", " & L_Nxt);
                                          else
                                             Emit_Term (Ctx.E, "jmp " & L_Nxt);
                                          end if;
                                          Emit_Label (Ctx.E, L_Hit);
                                          Walk_Rec_CL (Rec_P, Rec_Pir, D (AS_COMP_LIST, VN));
                                          Emit_Term (Ctx.E, "jmp " & L_End);
                                          Emit_Label (Ctx.E, L_Nxt);
                                       end;
                                    end if;
                                 end loop;
                                 Emit_Term (Ctx.E, "jmp " & L_End);
                                 Emit_Label (Ctx.E, L_End);
                              end;
                           end if;
                        end;
                     end if;
                  end;
               end Walk_Rec_CL;
            begin
               if CTI_Top.Kind = TK_Record then
                  declare
                     BT : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, Cur_T));
                  begin
                     if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
                        Walk_Rec_CL (Cur_Ptr, To_String (CTI_Top.Pir_Type), D (SM_COMP_LIST, BT));
                     end if;
                  end;
               elsif CTI_Top.Kind = TK_Constrained_Array and then not CTI_Top.Is_Dynamic then
                  declare
                     ETI : constant Type_Info := Get_Type_Info (Ctx, CTI_Top.Elem_Type);
                     ESz : constant Natural := Elem_Storage_Size (ETI);
                     Total_Elems : Natural := 1;
                     Zero64 : constant String := Next_Reg (Ctx.E, "dc0");
                     L_Hdr  : constant String := Next_Label (Ctx.E, "dch");
                     L_Body : constant String := Next_Label (Ctx.E, "dcb");
                     L_End  : constant String := Next_Label (Ctx.E, "dce");
                     Idx_R  : constant String := Next_Reg (Ctx.E, "dci");
                     Cmp_R  : constant String := Next_Reg (Ctx.E, "dcc");
                     Off_R  : constant String := Next_Reg (Ctx.E, "dco");
                     EP_R   : constant String := Next_Reg (Ctx.E, "dcep");
                     Nxt_R  : constant String := Next_Reg (Ctx.E, "dcn");
                  begin
                     for D_I in 1 .. CTI_Top.NDims loop
                        if CTI_Top.Dims (D_I).Last >= CTI_Top.Dims (D_I).First then
                           Total_Elems := Total_Elems * Natural (CTI_Top.Dims (D_I).Last - CTI_Top.Dims (D_I).First + 1);
                        else
                           Total_Elems := 0;
                        end if;
                     end loop;
                     if Total_Elems > 0 then
                        Emit_Inst (Ctx.E, Zero64 & " = const.i64 0");
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Zero64 & ")");
                        Emit_Label (Ctx.E, L_Hdr, Idx_R & ": i64");
                        Emit_Inst (Ctx.E, Cmp_R & " = cmp_lt_s.i64 " & Idx_R & ", " & Img (Total_Elems));
                        Emit_Term (Ctx.E, "br " & Cmp_R & ", " & L_Body & ", " & L_End);
                        Emit_Label (Ctx.E, L_Body);
                        Emit_Inst (Ctx.E, Off_R & " = mul.i64 " & Idx_R & ", " & Img (ESz));
                        Emit_Inst (Ctx.E, EP_R & " = ptr_add.i64 " & Cur_Ptr & ", " & Off_R & " [inbounds]");
                        if ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic then
                           Clone_Dyn_Arr (EP_R, CTI_Top.Elem_Type, ETI);
                        else
                           Deep_Copy_Fields (EP_R, CTI_Top.Elem_Type);
                        end if;
                        Emit_Inst (Ctx.E, Nxt_R & " = add.i64 " & Idx_R & ", 1");
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nxt_R & ")");
                        Emit_Label (Ctx.E, L_End);
                     end if;
                  end;
               end if;
            end Deep_Copy_Fields;
         begin
            if Type_Has_Dyn_Arr (Dst_Type) then
               Deep_Copy_Fields (Dst_Ptr, Dst_Type);
            end if;
         end;
      elsif DTI.Kind = TK_Unconstrained_Array or else DTI.Is_Dynamic then
         -- Copy based on Src's dynamic length across all dimensions (with super-null-safe overflow check + deep copy!)
         declare
            ETI   : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
            Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
            EAl   : constant Natural := Elem_Storage_Align (ETI);
            ESz   : constant Natural := Elem_Storage_Size (ETI);
            ND    : constant Natural := Natural'Max (1, Natural'Min ((if DTI.NDims > 0 then DTI.NDims else 1), Max_Array_Dims));
            SF, SL : Str_Array_4;
            Tot64 : Unbounded_String;
            Byt   : Unbounded_String;
         begin
            for D_Idx in 1 .. ND loop
               Get_Array_Dim_Bounds (Ctx, Src, D_Idx, SF (D_Idx), SL (D_Idx));
            end loop;
            Emit_Array_Byte_Size_I64 (Ctx, SF, SL, ND, ESz, Tot64, Byt);
            Emit_Inst (Ctx.E, "memmove " & Dst_Ptr & ", " & To_String (Src.Reg) & ", " & To_String (Byt) & ", align " & Img (EAl));
            -- Deep-copy any embedded dynamic constrained array components in each copied element!
            if Is_DE or else ETI.Kind = TK_Record or else ETI.Kind = TK_Constrained_Array then
               declare
                  Tmp_E : Expr_Val;
                  Zero64 : constant String := Next_Reg (Ctx.E, "duc0");
                  L_Hdr  : constant String := Next_Label (Ctx.E, "duch");
                  L_Body : constant String := Next_Label (Ctx.E, "ducb");
                  L_End  : constant String := Next_Label (Ctx.E, "duce");
                  Idx_R  : constant String := Next_Reg (Ctx.E, "duci");
                  Cmp_R  : constant String := Next_Reg (Ctx.E, "ducc");
                  Off_R  : constant String := Next_Reg (Ctx.E, "duco");
                  EP_R   : constant String := Next_Reg (Ctx.E, "ducep");
                  Nxt_R  : constant String := Next_Reg (Ctx.E, "ducn");
               begin
                  if Is_DE then
                     Emit_Inst (Ctx.E, Zero64 & " = const.i64 0");
                     Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Zero64 & ")");
                     Emit_Label (Ctx.E, L_Hdr, Idx_R & ": i64");
                     Emit_Inst (Ctx.E, Cmp_R & " = cmp_lt_s.i64 " & Idx_R & ", " & To_String (Tot64));
                     Emit_Term (Ctx.E, "br " & Cmp_R & ", " & L_Body & ", " & L_End);
                     Emit_Label (Ctx.E, L_Body);
                     Emit_Inst (Ctx.E, Off_R & " = mul.i64 " & Idx_R & ", " & Img (ESz));
                     Emit_Inst (Ctx.E, EP_R & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                     -- Clone each embedded @__ada_arr_desc in-place
                     declare
                        Orig  : Expr_Val;
                        ET2   : constant Type_Info := Get_Type_Info (Ctx, ETI.Elem_Type);
                        EAl2  : constant Natural := Elem_Storage_Align (ET2);
                        ESz2  : constant Natural := Elem_Storage_Size (ET2);
                        ND2   : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                        T2, B2 : Unbounded_String;
                        Nz2   : constant String := Next_Reg (Ctx.E, "dnz2");
                        Al2   : constant String := Next_Reg (Ctx.E, "dal2");
                        NP2   : Unbounded_String;
                        P0_2  : constant String := Next_Reg (Ctx.E, "dp02");
                     begin
                        Emit_Unpack_Arr_Desc (Ctx, EP_R, DTI.Elem_Type, Orig);
                        Emit_Array_Byte_Size_I64 (Ctx, Orig.First_Regs, Orig.Last_Regs, ND2, ESz2, T2, B2);
                        Emit_Inst (Ctx.E, Nz2 & " = cmp_gt_s.i64 " & To_String (B2) & ", 0");
                        Emit_Inst (Ctx.E, Al2 & " = select.i64 " & Nz2 & ", " & To_String (B2) & ", " & Img (EAl2));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", Al2, "ptr", NP2);
                        Emit_Inst (Ctx.E, "memcpy " & To_String (NP2) & ", " & To_String (Orig.Reg) & ", " & To_String (B2) & ", align " & Img (EAl2));
                        Emit_Inst (Ctx.E, P0_2 & " = field_ptr @__ada_arr_desc, " & EP_R & ", 0");
                        Emit_Inst (Ctx.E, "store.ptr " & To_String (NP2) & ", " & P0_2 & ", align 8");
                     end;
                     Emit_Inst (Ctx.E, Nxt_R & " = add.i64 " & Idx_R & ", 1");
                     Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nxt_R & ")");
                     Emit_Label (Ctx.E, L_End);
                  elsif ETI.Kind = TK_Record or else (ETI.Kind = TK_Constrained_Array and then not ETI.Is_Dynamic) then
                     -- Only emit loop if element type actually contains a dynamic array!
                     declare
                        function Rec_Or_Arr_Has_Dyn (T : TREE; Dep : Natural := 0) return Boolean is
                           T_I : Type_Info;
                           BT  : TREE;
                           function CL_Dyn (CL : TREE) return Boolean is
                           begin
                              if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return False; end if;
                              if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                                 declare
                                    DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                                    DN : TREE;
                                 begin
                                    while not IS_EMPTY (DL) loop
                                       POP (DL, DN);
                                       if KIND (DN) = DN_VARIABLE_DECL then
                                          declare
                                             NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DN));
                                             CID : TREE;
                                          begin
                                             while not IS_EMPTY (NS) loop
                                                POP (NS, CID);
                                                if Rec_Or_Arr_Has_Dyn (Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID)), Dep + 1) then
                                                   return True;
                                                end if;
                                             end loop;
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
                                          if KIND (VN) = DN_VARIANT and then CL_Dyn (D (AS_COMP_LIST, VN)) then
                                             return True;
                                          end if;
                                       end loop;
                                    end;
                                 end if;
                              end;
                              return False;
                           end CL_Dyn;
                        begin
                           if Dep > 16 or else Is_Void_Tree (T) then return False; end if;
                           T_I := Get_Type_Info (Ctx, T);
                           if T_I.Kind = TK_Constrained_Array and then T_I.Is_Dynamic then return True;
                           elsif T_I.Kind = TK_Constrained_Array then return Rec_Or_Arr_Has_Dyn (T_I.Elem_Type, Dep + 1);
                           elsif T_I.Kind = TK_Record then
                              BT := Base_Type_Spec (Subst_Tree (Ctx, T));
                              if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
                                 return CL_Dyn (D (SM_COMP_LIST, BT));
                              end if;
                           end if;
                           return False;
                        end Rec_Or_Arr_Has_Dyn;
                     begin
                        if Rec_Or_Arr_Has_Dyn (DTI.Elem_Type) then
                           Emit_Inst (Ctx.E, Zero64 & " = const.i64 0");
                           Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Zero64 & ")");
                           Emit_Label (Ctx.E, L_Hdr, Idx_R & ": i64");
                           Emit_Inst (Ctx.E, Cmp_R & " = cmp_lt_s.i64 " & Idx_R & ", " & To_String (Tot64));
                           Emit_Term (Ctx.E, "br " & Cmp_R & ", " & L_Body & ", " & L_End);
                           Emit_Label (Ctx.E, L_Body);
                           Emit_Inst (Ctx.E, Off_R & " = mul.i64 " & Idx_R & ", " & Img (ESz));
                           Emit_Inst (Ctx.E, EP_R & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                           Tmp_E.Kind := EK_Addr; Tmp_E.Reg := To_Unbounded_String (EP_R);
                           Tmp_E.Pir_Type := ETI.Pir_Type; Tmp_E.Ada_Type := DTI.Elem_Type; Tmp_E.Align := EAl;
                           Emit_Copy_Composite (Ctx, EP_R, Tmp_E, DTI.Elem_Type);
                           Emit_Inst (Ctx.E, Nxt_R & " = add.i64 " & Idx_R & ", 1");
                           Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nxt_R & ")");
                           Emit_Label (Ctx.E, L_End);
                        end if;
                     end;
                  end if;
               end;
            end if;
         end;
      end if;
   end Emit_Copy_Composite;

   procedure Emit_Init_Composite
     (Ctx          : in out Context;
      Dst_Ptr      : String;
      Dst_Type     : TREE;
      Init_Node    : TREE;
      Dst_Firsts   : Str_Array_4 := (others => Null_Unbounded_String);
      Dst_Lasts    : Str_Array_4 := (others => Null_Unbounded_String);
      Exact_Bounds : Boolean := False)
   is
      DTI : constant Type_Info := Get_Type_Info (Ctx, Dst_Type);
   begin
      if Is_Void_Tree (Init_Node) then
         return;
      end if;
      if KIND (Init_Node) = DN_QUALIFIED then
         declare
            Qual_T : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Init_Node));
            QTI    : constant Type_Info := (if not Is_Void_Tree (Qual_T) then Get_Type_Info (Ctx, Qual_T) else DTI);
         begin
            if not Is_Void_Tree (Qual_T) and then QTI.Kind = TK_Constrained_Array
              and then DTI.Kind in TK_Constrained_Array | TK_Unconstrained_Array
            then
               -- P3-3.6: Evaluate qualified array with Qual_T first (enforcing Qual_T's exact bounds),
               -- then check/slide into Dst_Type!
               declare
                  Src  : constant Expr_Val := Emit_Expr (Ctx, Init_Node, Qual_T);
                  ND_A : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
                  Tgt  : Expr_Val;
               begin
                  if Ctx.Enable_Checks then
                     Tgt.Kind := EK_Slice; Tgt.Ada_Type := Dst_Type; Tgt.NDims := ND_A;
                     for D_I in 1 .. ND_A loop
                        if Length (Dst_Firsts (D_I)) > 0 then
                           Tgt.First_Regs (D_I) := Dst_Firsts (D_I);
                           Tgt.Last_Regs (D_I)  := Dst_Lasts (D_I);
                        elsif D_I <= DTI.NDims and then not Is_Void_Tree (DTI.Dims (D_I).Idx_Type) then
                           Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                        end if;
                     end loop;
                     if Exact_Bounds then
                        declare
                           AV : constant Array_View := Resolve_Array_View (Ctx, Src);
                        begin
                           Emit_Exact_Bounds_Check (Ctx, AV.Firsts, AV.Lasts, Tgt.First_Regs, Tgt.Last_Regs, ND_A);
                        end;
                     else
                        Emit_Array_Length_Check (Ctx, Tgt, Src, ND_A);
                     end if;
                  end if;
                  Emit_Copy_Composite (Ctx, Dst_Ptr, Src, Dst_Type);
               end;
            else
               Emit_Init_Composite (Ctx, Dst_Ptr, Dst_Type, D (AS_EXP, Init_Node), Dst_Firsts, Dst_Lasts, Exact_Bounds => True);
               if not Is_Void_Tree (Qual_T) and then QTI.Kind = TK_Record then
                  Emit_Discriminant_Check (Ctx, Dst_Ptr, Qual_T, Is_Access => False);
               end if;
            end if;
         end;
         return;
      end if;
      if KIND (Init_Node) = DN_PARENTHESIZED then
         if DTI.Kind = TK_Constrained_Array
           and then not Is_Void_Tree (D (AS_EXP, Init_Node))
           and then KIND (D (AS_EXP, Init_Node)) in DN_AGGREGATE | DN_STRING_LITERAL
         then
            -- LRM 4.3.2(5)+(9) (c43205f): parenthesized aggregate loses applicable index constraint
            -- (lower bound comes from base index subtype 'FIRST), and as an aggregate component must match Dst_Type bounds!
            declare
               Base_Arr : constant TREE := Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type)));
               Src      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Init_Node), Base_Arr);
               ND_A     : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
            begin
               if Ctx.Enable_Checks then
                  if Exact_Bounds then
                     declare
                        SV_W : constant Array_View := Resolve_Array_View (Ctx, Src);
                        DF, DL : Str_Array_4;
                     begin
                        for D_I in 1 .. ND_A loop
                           if Length (Dst_Firsts (D_I)) > 0 then
                              DF (D_I) := Dst_Firsts (D_I);
                              DL (D_I) := Dst_Lasts (D_I);
                           else
                              Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, DF (D_I), DL (D_I));
                           end if;
                        end loop;
                        Emit_Exact_Bounds_Check (Ctx, SV_W.Firsts, SV_W.Lasts, DF, DL, ND_A);
                     end;
                  else
                     declare
                        Tgt : Expr_Val;
                     begin
                        Tgt.Kind := EK_Slice; Tgt.Ada_Type := Dst_Type; Tgt.NDims := ND_A;
                        for D_I in 1 .. ND_A loop
                           if Length (Dst_Firsts (D_I)) > 0 then
                              Tgt.First_Regs (D_I) := Dst_Firsts (D_I);
                              Tgt.Last_Regs (D_I)  := Dst_Lasts (D_I);
                           else
                              Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                           end if;
                        end loop;
                        Emit_Array_Length_Check (Ctx, Tgt, Src, ND_A);
                     end;
                  end if;
               end if;
               Emit_Copy_Composite (Ctx, Dst_Ptr, Src, Dst_Type);
            end;
            return;
         end if;
         Emit_Init_Composite (Ctx, Dst_Ptr, Dst_Type, D (AS_EXP, Init_Node), Dst_Firsts, Dst_Lasts, Exact_Bounds => Exact_Bounds);
         return;
      end if;

      if KIND (Init_Node) = DN_AGGREGATE then
         if DTI.Kind = TK_Record then
            declare
               Norm     : TREE := D (SM_NORMALIZED_COMP_S, Init_Node);
               Comps    : SEQ_TYPE;
               C_Exp    : TREE;
               Base_Rec : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, Dst_Type));
               -- Collect component IDs in field order (discriminants + fixed + active variant)
               Comp_Ids    : array (0 .. 255) of TREE;
               N_Comps     : Natural := 0;
               Disc_Vals   : array (0 .. 31) of Integer_64 := (others => 0);
               Disc_Static : array (0 .. 31) of Boolean := (others => False);
               Disc_Ids    : array (0 .. 31) of TREE := (others => CONST_VOID);
               N_Discs     : Natural := 0;
               Bound_Discs : Temp_Var_Scope;

               procedure Gather_Ids (DS : TREE; Is_Disc : Boolean := False) is
                  DL  : SEQ_TYPE;
                  D_N : TREE;
               begin
                  if Is_Void_Tree (DS) then return; end if;
                  DL := LIST (DS);
                  while not IS_EMPTY (DL) loop
                     POP (DL, D_N);
                     if KIND (D_N) = DN_VARIABLE_DECL or else KIND (D_N) = DN_DSCRMT_DECL then
                        declare
                           NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, D_N));
                           ID : TREE;
                        begin
                           while not IS_EMPTY (NS) loop
                              POP (NS, ID);
                              if N_Comps < 256 then
                                 Comp_Ids (N_Comps) := ID;
                                 N_Comps := N_Comps + 1;
                              end if;
                              if Is_Disc and then N_Discs < 32 then
                                 Disc_Ids (N_Discs) := ID;
                                 N_Discs := N_Discs + 1;
                              end if;
                           end loop;
                        end;
                     end if;
                  end loop;
               end Gather_Ids;

               function Match_Variant_Choice (Ch : TREE; Val : Integer_64) return Boolean is
               begin
                  if KIND (Ch) = DN_CHOICE_OTHERS then
                     return True;
                  elsif KIND (Ch) = DN_CHOICE_EXP then
                     declare
                        SV : constant TREE := Safe_SM_Value (D (AS_EXP, Ch));
                     begin
                        return Has_Static_Val (SV) and then Eval_Int64 (SV) = Val;
                     end;
                  elsif KIND (Ch) = DN_CHOICE_RANGE then
                     declare
                        DR : constant TREE := D (AS_DISCRETE_RANGE, Ch);
                     begin
                        if not Is_Void_Tree (DR) and then KIND (DR) = DN_RANGE then
                           declare
                              V1 : constant TREE := Safe_SM_Value (D (AS_EXP1, DR));
                              V2 : constant TREE := Safe_SM_Value (D (AS_EXP2, DR));
                           begin
                              return Has_Static_Val (V1) and then Has_Static_Val (V2)
                                and then Val >= Eval_Int64 (V1) and then Val <= Eval_Int64 (V2);
                           end;
                        elsif not Is_Void_Tree (DR) and then KIND (DR) = DN_DISCRETE_SUBTYPE then
                           declare
                              St : Boolean := False;
                              Lo, Hi : Integer_64 := 0;
                              Sub_N : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                              Con_N : constant TREE := (if not Is_Void_Tree (Sub_N) then D (AS_CONSTRAINT, Sub_N) else CONST_VOID);
                              TM    : constant TREE := (if not Is_Void_Tree (Sub_N) then Get_Name_Defn (D (AS_NAME, Sub_N)) else CONST_VOID);
                           begin
                              if not Is_Void_Tree (Con_N) and then KIND (Con_N) = DN_RANGE then
                                 declare
                                    V1 : constant TREE := Safe_SM_Value (D (AS_EXP1, Con_N));
                                    V2 : constant TREE := Safe_SM_Value (D (AS_EXP2, Con_N));
                                 begin
                                    if Has_Static_Val (V1) and then Has_Static_Val (V2) then
                                       return Val >= Eval_Int64 (V1) and then Val <= Eval_Int64 (V2);
                                    end if;
                                 end;
                              end if;
                              if not Is_Void_Tree (TM) and then KIND (TM) in CLASS_TYPE_NAME then
                                 Get_Discrete_Bounds (D (SM_TYPE_SPEC, TM), St, Lo, Hi);
                                 return St and then Val >= Lo and then Val <= Hi;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
                  return False;
               end Match_Variant_Choice;

               procedure Gather_Comp_List (CL : TREE) is
               begin
                  if Is_Void_Tree (CL) then return; end if;
                  Gather_Ids (D (AS_DECL_S, CL));
                  declare
                     VP : constant TREE := D (AS_VARIANT_PART, CL);
                  begin
                     if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART then
                        declare
                           DN_Name : constant TREE := D (AS_NAME, VP);
                           D_Def   : constant TREE := Canonical_Def (Get_Name_Defn (DN_Name));
                           D_Val   : Integer_64 := 0;
                           Has_Val : Boolean := False;
                           VS      : constant TREE := D (AS_VARIANT_S, VP);
                           VL      : SEQ_TYPE := (if not Is_Void_Tree (VS) then LIST (VS) else MAKE);
                           VN      : TREE;
                           Matched : TREE := CONST_VOID;
                           Others_V : TREE := CONST_VOID;
                        begin
                           for I in 0 .. N_Discs - 1 loop
                              if Canonical_Def (Disc_Ids (I)) = D_Def and then Disc_Static (I) then
                                 D_Val := Disc_Vals (I);
                                 Has_Val := True;
                                 exit;
                              end if;
                           end loop;
                           while not IS_EMPTY (VL) loop
                              POP (VL, VN);
                              if KIND (VN) = DN_VARIANT then
                                 declare
                                    CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN));
                                    Ch : TREE;
                                 begin
                                    while not IS_EMPTY (CS) loop
                                       POP (CS, Ch);
                                       if KIND (Ch) = DN_CHOICE_OTHERS then
                                          Others_V := VN;
                                       elsif Has_Val and then Match_Variant_Choice (Ch, D_Val) then
                                          Matched := VN;
                                          exit;
                                       end if;
                                    end loop;
                                 end;
                                 exit when not Is_Void_Tree (Matched);
                              end if;
                           end loop;
                           if Is_Void_Tree (Matched) then
                              Matched := Others_V;
                           end if;
                           if not Is_Void_Tree (Matched) then
                              Gather_Comp_List (D (AS_COMP_LIST, Matched));
                           end if;
                        end;
                     end if;
                  end;
               end Gather_Comp_List;

               function Emit_Field_Ptr_For (CID : TREE; Fallback_Idx : Natural) return String is
                  CK : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                  FP : constant String := Next_Reg (Ctx.E, "afp");
               begin
                  if Ctx.Fields.Contains (CK) then
                     declare
                        FE : constant Field_Entry := Ctx.Fields.Element (CK);
                     begin
                        if not FE.In_Union and then not FE.Has_Explicit_Offset then
                           Emit_Inst (Ctx.E,
                             FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " & Dst_Ptr & ", " & Img (FE.Field_Idx));
                        else
                           declare
                              Off_R : constant String := Next_Reg (Ctx.E, "afo");
                           begin
                              Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                              Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                           end;
                        end if;
                     end;
                  else
                     Emit_Inst (Ctx.E,
                       FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " & Dst_Ptr & ", " & Img (Fallback_Idx));
                  end if;
                  return FP;
               end Emit_Field_Ptr_For;
            begin
               if KIND (Base_Rec) /= DN_RECORD then
                  return;
               end if;
               if Is_Void_Tree (Norm) then
                  Norm := D (AS_GENERAL_ASSOC_S, Init_Node);
               end if;
               Gather_Ids (D (SM_DISCRIMINANT_S, Base_Rec), Is_Disc => True);
               -- Peek static discriminant values from Norm so Gather_Comp_List selects the active variant!
               declare
                  Peek : SEQ_TYPE := LIST (Norm);
                  PE   : TREE;
                  DI_I : Natural := 0;
               begin
                  while not IS_EMPTY (Peek) and then DI_I < N_Discs loop
                     POP (Peek, PE);
                     if KIND (PE) = DN_ASSOC or else KIND (PE) = DN_NAMED then
                        PE := D (AS_EXP, PE);
                     end if;
                     if not Is_Void_Tree (PE) and then Has_Static_Val (Safe_SM_Value (PE)) then
                        Disc_Vals (DI_I) := Eval_Int64 (Safe_SM_Value (PE));
                        Disc_Static (DI_I) := True;
                     end if;
                     DI_I := DI_I + 1;
                  end loop;
               end;
               if not Is_Void_Tree (D (SM_COMP_LIST, Base_Rec)) then
                  Gather_Comp_List (D (SM_COMP_LIST, Base_Rec));
               end if;
               -- Zero-init record buffer first so padding/inactive union bytes are deterministic
               if DTI.Byte_Size > 0 then
                  Emit_Inst (Ctx.E, "memset " & Dst_Ptr & ", 0, " & Img (DTI.Byte_Size) & ", align " & Img (Natural'Max (1, DTI.Align)));
               end if;
               Comps := LIST (Norm);
               declare
                  Idx : Natural := 0;
               begin
                  while not IS_EMPTY (Comps) and then Idx < N_Comps loop
                     POP (Comps, C_Exp);
                     if KIND (C_Exp) = DN_ASSOC or else KIND (C_Exp) = DN_NAMED then
                        C_Exp := D (AS_EXP, C_Exp);
                     end if;
                     declare
                        CID : constant TREE := Comp_Ids (Idx);
                        CID_K : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                        CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                        RT_C : constant TREE := Resolve_Type_Spec (CT);
                        CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                        FP  : constant String := Emit_Field_Ptr_For (CID, Idx);
                        Has_SD_Save : Boolean := False;
                        Prev_SD     : Saved_Disc_Info;
                        Has_SR_Save : Boolean := False;
                        Prev_SR     : Saved_Range_Info;
                        SR_Key      : Unsigned_32 := 0;
                     begin
                        if not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Discs.Contains (CID_K) then
                           if Ctx.Saved_Type_Discs.Contains (Tree_Key (RT_C)) then
                              Has_SD_Save := True;
                              Prev_SD := Ctx.Saved_Type_Discs.Element (Tree_Key (RT_C));
                           end if;
                           Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Ctx.Saved_Type_Discs.Element (CID_K));
                        elsif not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Ranges.Contains (CID_K) then
                           if KIND (RT_C) = DN_CONSTRAINED_ARRAY and then CTI.NDims >= 1 and then not Is_Void_Tree (CTI.Dims (1).Idx_Type) then
                              SR_Key := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, CTI.Dims (1).Idx_Type)));
                           else
                              SR_Key := Tree_Key (RT_C);
                           end if;
                           if Ctx.Saved_Type_Ranges.Contains (SR_Key) then
                              Has_SR_Save := True;
                              Prev_SR := Ctx.Saved_Type_Ranges.Element (SR_Key);
                           end if;
                           Ctx.Saved_Type_Ranges.Include (SR_Key, Ctx.Saved_Type_Ranges.Element (CID_K));
                        end if;
                        if CTI.Kind = TK_Scalar then
                           declare
                              V     : constant Expr_Val := Emit_Expr (Ctx, C_Exp, CT);
                              VP    : constant String := To_String (V.Pir_Type);
                              DP    : constant String := To_String (CTI.Pir_Type);
                              VU    : constant Boolean := Is_Unsigned_Scalar (V.Ada_Type, VP);
                              VR    : Unbounded_String;
                              CK_F  : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                           begin
                              if Int_Bits_Of_Pir (VP) > Int_Bits_Of_Pir (DP) and then Is_Discrete_Type (CT) then
                                 Emit_Range_Check (Ctx, To_String (V.Reg), VP, CT);
                              end if;
                              VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (V.Reg), VP, DP, Is_Unsigned => VU));
                              Emit_Range_Check (Ctx, To_String (VR), DP, CT);
                              Emit_Discriminant_Check (Ctx, To_String (VR), CT, Is_Access => True);
                              if Ctx.Fields.Contains (CK_F) and then Is_Bitfield (Ctx.Fields.Element (CK_F)) then
                                 Emit_Bitfield_Store (Ctx, Dst_Ptr, Ctx.Fields.Element (CK_F), To_String (VR), DP, CT);
                              else
                                 declare
                                    St_Al : constant Natural :=
                                      (if Ctx.Fields.Contains (CK_F)
                                       then Explicit_Field_Align (Ctx.Fields.Element (CK_F), CTI.Align)
                                       else Natural'Max (1, CTI.Align));
                                 begin
                                    Emit_Inst (Ctx.E,
                                      "store." & DP & " " & To_String (VR) &
                                      ", " & FP & ", align " & Img (St_Al));
                                 end;
                              end if;
                           end;
                           -- If this is a discriminant, bind it in Ctx.Vars for subsequent dynamic-array components!
                           if Idx < N_Discs then
                              declare
                                 DK : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                 DV : Var_Info;
                              begin
                                 DV.Kind       := VK_Local_Alloca;
                                 DV.Pir_Type   := CTI.Pir_Type;
                                 DV.Ada_Type   := CT;
                                 DV.Owner_Subp := Ctx.Cur_Subp_Key;
                                 if Ctx.Fields.Contains (DK) and then Is_Bitfield (Ctx.Fields.Element (DK)) then
                                    DV.Pir_Name    := To_Unbounded_String (Dst_Ptr);
                                    DV.Is_Bitfield := True;
                                    DV.BF_FE       := Ctx.Fields.Element (DK);
                                    DV.Align       := 1;
                                 else
                                    DV.Pir_Name := To_Unbounded_String (FP);
                                    DV.Align    :=
                                      (if Ctx.Fields.Contains (DK)
                                       then Explicit_Field_Align (Ctx.Fields.Element (DK), CTI.Align)
                                       else CTI.Align);
                                 end if;
                                 Bind_Temp_Var (Ctx, Bound_Discs, DK, DV);
                              end;
                           end if;
                        elsif CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                           -- Embedded @__ada_arr_desc for dynamic constrained array field (RC-8)
                           declare
                              ETI   : constant Type_Info := Get_Type_Info (Ctx, CTI.Elem_Type);
                              EAl   : constant Natural := Elem_Storage_Align (ETI);
                              ESz   : constant Natural := Elem_Storage_Size (ETI);
                              ND_A  : constant Natural := Natural'Max (1, Natural'Min (CTI.NDims, Max_Array_Dims));
                              pragma Unreferenced (EAl);
                              Firsts, Lasts : Str_Array_4;
                              Tot64 : Unbounded_String;
                              Byt   : constant String := Next_Reg (Ctx.E, "raby");
                              Nz    : constant String := Next_Reg (Ctx.E, "ranz");
                              AlB   : constant String := Next_Reg (Ctx.E, "raal");
                              DataP : Unbounded_String;
                           begin
                              for D_I in 1 .. ND_A loop
                                 Emit_Index_Bounds (Ctx, CTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                                 if Ctx.Enable_Checks then
                                    declare
                                       BA  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (CT));
                                       BTI : constant Type_Info := (if not Is_Void_Tree (BA) and then KIND (BA) = DN_ARRAY then Get_Type_Info (Ctx, BA) else CTI);
                                    begin
                                       if D_I <= BTI.NDims and then not Is_Void_Tree (BTI.Dims (D_I).Idx_Type) then
                                          Emit_Non_Null_Index_Check (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)), BTI.Dims (D_I).Idx_Type);
                                       end if;
                                    end;
                                 end if;
                                 declare
                                    DL : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)));
                                 begin
                                    if D_I = 1 then Tot64 := To_Unbounded_String (DL);
                                    else
                                       declare
                                          M : constant String := Next_Reg (Ctx.E, "ram");
                                       begin
                                          Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & DL);
                                          Tot64 := To_Unbounded_String (M);
                                       end;
                                    end if;
                                 end;
                              end loop;
                              Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                              Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                              Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                              Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
                              Emit_Store_Arr_Desc (Ctx, FP, To_String (DataP), Firsts, Lasts, ND_A);
                              if KIND (C_Exp) = DN_AGGREGATE then
                                 declare
                                    function Agg_All_Named_No_Others (AN : TREE; Rem_D : Natural) return Boolean is
                                    begin
                                       if Rem_D = 0 then return True; end if;
                                       if Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then return False; end if;
                                       declare
                                          AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                                          IT : TREE;
                                          Has_Any : Boolean := False;
                                       begin
                                          while not IS_EMPTY (AL) loop
                                             POP (AL, IT);
                                             Has_Any := True;
                                             if KIND (IT) not in DN_ASSOC | DN_NAMED then
                                                return False;
                                             end if;
                                             declare
                                                CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                                Ch : TREE;
                                             begin
                                                while not IS_EMPTY (CS) loop
                                                   POP (CS, Ch);
                                                   if KIND (Ch) = DN_CHOICE_OTHERS then return False; end if;
                                                end loop;
                                             end;
                                             if Rem_D > 1 and then not Agg_All_Named_No_Others (D (AS_EXP, IT), Rem_D - 1) then
                                                return False;
                                             end if;
                                          end loop;
                                          return Has_Any;
                                       end;
                                    end Agg_All_Named_No_Others;
                                 begin
                                    if Agg_All_Named_No_Others (C_Exp, ND_A) then
                                       -- LRM 4.3.1(3) + 4.3.2(8) (c43103b Case 5): evaluate all dim choices via Pass-1 and check exact bounds!
                                       declare
                                          Base_CT : constant TREE := Subst_Base_Type (Ctx, CT);
                                          SV      : constant Expr_Val := Emit_Expr (Ctx, C_Exp, Base_CT);
                                          SV_W    : constant Array_View := Resolve_Array_View (Ctx, SV);
                                       begin
                                          Emit_Exact_Bounds_Check (Ctx, SV_W.Firsts, SV_W.Lasts, Firsts, Lasts, ND_A);
                                          Emit_Inst (Ctx.E, "memmove " & To_String (DataP) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                                       end;
                                    else
                                       Emit_Init_Composite (Ctx, To_String (DataP), CT, C_Exp, Firsts, Lasts, Exact_Bounds => True);
                                    end if;
                                 end;
                              else
                                 declare
                                    Is_Par : constant Boolean := KIND (C_Exp) = DN_PARENTHESIZED;
                                    Eval_T : constant TREE :=
                                      (if Is_Par then Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, CT))) else CT);
                                    SV  : constant Expr_Val := Emit_Expr (Ctx, C_Exp, Eval_T);
                                    Tgt : Expr_Val;
                                 begin
                                    Tgt.Kind := EK_Slice; Tgt.Reg := DataP; Tgt.Ada_Type := CT;
                                    Tgt.NDims := ND_A; Tgt.First_Regs := Firsts; Tgt.Last_Regs := Lasts;
                                    if not Is_Par and then KIND (C_Exp) = DN_STRING_LITERAL then
                                       Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                                    elsif Ctx.Enable_Checks then
                                       -- LRM 4.3.1(3) (c43004b, c43103b): record aggregate constrained array component requires EXACT bounds (no sliding)!
                                       declare
                                          SV_W : constant Array_View := Resolve_Array_View (Ctx, SV);
                                       begin
                                          Emit_Exact_Bounds_Check (Ctx, SV_W.Firsts, SV_W.Lasts, Firsts, Lasts, ND_A);
                                       end;
                                    end if;
                                    Emit_Inst (Ctx.E, "memmove " & To_String (DataP) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                                 end;
                              end if;
                           end;
                        else
                           declare
                              Need_Exact : constant Boolean :=
                                CTI.Kind = TK_Constrained_Array
                                and then KIND (C_Exp) not in DN_AGGREGATE | DN_STRING_LITERAL;
                           begin
                              Emit_Init_Composite (Ctx, FP, CT, C_Exp, Exact_Bounds => Need_Exact);
                           end;
                           if CTI.Kind = TK_Record then
                              Emit_Discriminant_Check (Ctx, FP, CT, Is_Access => False);
                           end if;
                        end if;
                        if not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Discs.Contains (CID_K) then
                           if Has_SD_Save then
                              Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Prev_SD);
                           else
                              Ctx.Saved_Type_Discs.Exclude (Tree_Key (RT_C));
                           end if;
                        end if;
                        if SR_Key /= 0 then
                           if Has_SR_Save then
                              Ctx.Saved_Type_Ranges.Include (SR_Key, Prev_SR);
                           else
                              Ctx.Saved_Type_Ranges.Exclude (SR_Key);
                           end if;
                        end if;
                     end;
                     Idx := Idx + 1;
                  end loop;
               end;
               Restore_Temp_Vars (Ctx, Bound_Discs);
            end;
            return;

         elsif DTI.Kind = TK_Constrained_Array or else DTI.Kind = TK_Unconstrained_Array then
            declare
               ETI    : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
               ND_Tot : constant Natural := Natural'Max (1, DTI.NDims);

               procedure Get_Dim_Lo_Hi
                 (Dim : Natural;
                  Lo  : out Unbounded_String;
                  Hi  : out Unbounded_String)
               is
               begin
                  if Dim <= Max_Array_Dims and then Length (Dst_Firsts (Dim)) > 0 then
                     Lo := Dst_Firsts (Dim);
                     Hi := Dst_Lasts (Dim);
                  elsif DTI.Kind = TK_Constrained_Array and then Dim <= DTI.NDims and then DTI.Dims (Dim).Is_Static then
                     Lo := To_Unbounded_String (Emit_Int_Const (Ctx, DTI.Dims (Dim).First, "i64", "af"));
                     Hi := To_Unbounded_String (Emit_Int_Const (Ctx, DTI.Dims (Dim).Last, "i64", "al"));
                  else
                     declare
                        RF : constant String := Next_Reg (Ctx.E, "af");
                        RL : constant String := Next_Reg (Ctx.E, "al");
                     begin
                        Emit_Inst (Ctx.E, RF & " = const.i64 1");
                        Emit_Inst (Ctx.E, RL & " = const.i64 0");
                        Lo := To_Unbounded_String (RF);
                        Hi := To_Unbounded_String (RL);
                     end;
                  end if;
               end Get_Dim_Lo_Hi;

               function Compute_Sub_Stride (Dim : Natural) return String is
                  Acc : Unbounded_String;
                  One : constant String := Next_Reg (Ctx.E, "st1");
               begin
                  Emit_Inst (Ctx.E, One & " = const.i64 1");
                  Acc := To_Unbounded_String (One);
                  for D_I in Dim + 1 .. ND_Tot loop
                     declare
                        FL, FH : Unbounded_String;
                        C6 : Unbounded_String;
                        Nx : constant String := Next_Reg (Ctx.E, "sm");
                     begin
                        Get_Dim_Lo_Hi (D_I, FL, FH);
                        C6 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (FL), To_String (FH)));
                        Emit_Inst (Ctx.E, Nx & " = mul.i64 " & To_String (Acc) & ", " & To_String (C6));
                        Acc := To_Unbounded_String (Nx);
                     end;
                  end loop;
                  return To_String (Acc);
               end Compute_Sub_Stride;

               procedure Emit_Init_Array_Dim
                 (Cur_Ptr  : String;
                  Agg_Node : TREE;
                  Dim      : Natural)
               is
                  Assocs : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, Agg_Node));
                  Item   : TREE;
                  Pos    : Integer_64 := 0;
                  Lo_Reg : Unbounded_String;
                  Hi_Reg : Unbounded_String;
                  Stride : constant String := (if Dim < ND_Tot then Compute_Sub_Stride (Dim) else "");

                  procedure Store_Elem_At (Off64 : String; Val_Node : TREE) is
                     EP : constant String := Next_Reg (Ctx.E, "aep");
                  begin
                     if Dim < ND_Tot then
                        declare
                           Scaled : constant String := Next_Reg (Ctx.E, "asc");
                           EP_S   : Unbounded_String;
                           Unw_V  : TREE := Val_Node;
                        begin
                           while not Is_Void_Tree (Unw_V) and then KIND (Unw_V) = DN_PARENTHESIZED loop
                              Unw_V := D (AS_EXP, Unw_V);
                           end loop;
                           Emit_Inst (Ctx.E, Scaled & " = mul.i64 " & Off64 & ", " & Stride);
                           EP_S := To_Unbounded_String (Emit_Elem_Ptr (Ctx, ETI, Cur_Ptr, Scaled));
                           if not Is_Void_Tree (Unw_V) and then KIND (Unw_V) = DN_AGGREGATE then
                              Emit_Init_Array_Dim (To_String (EP_S), Unw_V, Dim + 1);
                           elsif not Is_Void_Tree (Unw_V) and then KIND (Unw_V) = DN_STRING_LITERAL then
                              declare
                                 SV  : constant Expr_Val := Emit_Expr (Ctx, Unw_V);
                                 ESz : constant Natural := Elem_Storage_Size (ETI);
                                 Byt : constant String := Next_Reg (Ctx.E, "sby");
                              begin
                                 -- P3-3.8: Check multi-D string sub-aggregate length against dim+1 length!
                                 if Ctx.Enable_Checks then
                                    declare
                                       D2Lo, D2Hi, SF, SL : Unbounded_String;
                                       D2Len, SLen : Unbounded_String;
                                       Mis : constant String := Next_Reg (Ctx.E, "smis");
                                    begin
                                       Get_Dim_Lo_Hi (Dim + 1, D2Lo, D2Hi);
                                       Get_Array_Dim_Bounds (Ctx, SV, 1, SF, SL);
                                       D2Len := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (D2Lo), To_String (D2Hi)));
                                       SLen  := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (SF), To_String (SL)));
                                       Emit_Inst (Ctx.E, Mis & " = cmp_ne.i64 " & To_String (SLen) & ", " & To_String (D2Len));
                                       Emit_Check_Fail_If (Ctx, Mis);
                                    end;
                                 end if;
                                 Emit_Inst (Ctx.E, Byt & " = mul.i64 " & Stride & ", " & Img (ESz));
                                 Emit_Inst (Ctx.E, "memcpy " & To_String (EP_S) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                              end;
                           end if;
                        end;
                        pragma Unreferenced (EP);
                     else
                        declare
                           EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, Cur_Ptr, Off64);
                        begin
                           if ETI.Kind = TK_Scalar then
                              declare
                                 V  : constant Expr_Val := Emit_Expr (Ctx, Val_Node, DTI.Elem_Type);
                                 VR : constant String :=
                                   Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (ETI.Pir_Type));
                              begin
                                 Emit_Range_Check (Ctx, VR, To_String (ETI.Pir_Type), DTI.Elem_Type);
                                 Emit_Discriminant_Check (Ctx, VR, DTI.Elem_Type, Is_Access => True);
                                 Emit_Inst (Ctx.E,
                                   "store." & To_String (ETI.Pir_Type) & " " & VR &
                                   ", " & EP2 & ", align " & Img (ETI.Align));
                              end;
                           elsif ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic then
                              -- Array-of-dynamic-array element: allocate buffer, store @__ada_arr_desc at EP2, init/copy!
                              declare
                                 IETI   : constant Type_Info := Get_Type_Info (Ctx, ETI.Elem_Type);
                                 IIs_DE : constant Boolean := IETI.Kind = TK_Constrained_Array and then IETI.Is_Dynamic;
                                 IEAl   : constant Natural := (if IIs_DE then 8 else Natural'Max (1, IETI.Align));
                                 IESz   : constant Natural := Elem_Storage_Size (IETI);
                                 IND_A  : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                                 IFirsts, ILasts : Str_Array_4;
                                 ITot64 : Unbounded_String;
                                 IByt   : constant String := Next_Reg (Ctx.E, "aaby");
                                 INz    : constant String := Next_Reg (Ctx.E, "aanz");
                                 IAlB   : constant String := Next_Reg (Ctx.E, "aaal");
                                 IDataP : Unbounded_String;
                              begin
                                 for D_I in 1 .. IND_A loop
                                    Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, IFirsts (D_I), ILasts (D_I));
                                    declare
                                       DL : constant String := Emit_Dim_Len_I64 (Ctx, To_String (IFirsts (D_I)), To_String (ILasts (D_I)));
                                    begin
                                       if D_I = 1 then ITot64 := To_Unbounded_String (DL);
                                       else
                                          declare
                                             M : constant String := Next_Reg (Ctx.E, "aam");
                                          begin
                                             Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (ITot64) & ", " & DL);
                                             ITot64 := To_Unbounded_String (M);
                                          end;
                                       end if;
                                    end;
                                 end loop;
                                 Emit_Inst (Ctx.E, IByt & " = mul.i64 " & To_String (ITot64) & ", " & Img (IESz));
                                 Emit_Inst (Ctx.E, INz & " = cmp_gt_s.i64 " & IByt & ", 0");
                                 Emit_Inst (Ctx.E, IAlB & " = select.i64 " & INz & ", " & IByt & ", " & Img (IEAl));
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", IAlB, "ptr", IDataP);
                                 Emit_Store_Arr_Desc (Ctx, EP2, To_String (IDataP), IFirsts, ILasts, IND_A);
                                 if not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_AGGREGATE then
                                    Emit_Init_Composite (Ctx, To_String (IDataP), DTI.Elem_Type, Val_Node, IFirsts, ILasts);
                                 else
                                    declare
                                       Is_Par : constant Boolean := not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_PARENTHESIZED;
                                       Eval_T : constant TREE :=
                                         (if Is_Par then Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, DTI.Elem_Type)))
                                          else DTI.Elem_Type);
                                       SV  : constant Expr_Val := Emit_Expr (Ctx, Val_Node, Eval_T);
                                       Tgt : Expr_Val;
                                    begin
                                       Tgt.Kind := EK_Slice; Tgt.Reg := IDataP; Tgt.Ada_Type := DTI.Elem_Type;
                                       Tgt.NDims := IND_A; Tgt.First_Regs := IFirsts; Tgt.Last_Regs := ILasts;
                                       if not Is_Par and then not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_STRING_LITERAL then
                                          Emit_Array_Length_Check (Ctx, Tgt, SV, IND_A);
                                       elsif Ctx.Enable_Checks then
                                          declare
                                             SV_W : constant Array_View := Resolve_Array_View (Ctx, SV);
                                          begin
                                             Emit_Exact_Bounds_Check (Ctx, SV_W.Firsts, SV_W.Lasts, IFirsts, ILasts, IND_A);
                                          end;
                                       end if;
                                       Emit_Inst (Ctx.E, "memmove " & To_String (IDataP) & ", " & To_String (SV.Reg) & ", " & IByt & ", align " & Img (IEAl));
                                    end;
                                 end if;
                              end;
                           else
                              declare
                                 function Comp_Agg_All_Named (AN : TREE; Rem_D : Natural) return Boolean is
                                 begin
                                    if Rem_D = 0 then return True; end if;
                                    if Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then return False; end if;
                                    declare
                                       AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                                       IT : TREE;
                                       Has_Any : Boolean := False;
                                    begin
                                       while not IS_EMPTY (AL) loop
                                          POP (AL, IT);
                                          Has_Any := True;
                                          if KIND (IT) not in DN_ASSOC | DN_NAMED then return False; end if;
                                          declare
                                             CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                             Ch : TREE;
                                          begin
                                             while not IS_EMPTY (CS) loop
                                                POP (CS, Ch);
                                                if KIND (Ch) = DN_CHOICE_OTHERS then return False; end if;
                                             end loop;
                                          end;
                                          if Rem_D > 1 and then not Comp_Agg_All_Named (D (AS_EXP, IT), Rem_D - 1) then
                                             return False;
                                          end if;
                                       end loop;
                                       return Has_Any;
                                    end;
                                 end Comp_Agg_All_Named;
                                 Need_Exact : constant Boolean :=
                                   ETI.Kind = TK_Constrained_Array
                                   and then not Is_Void_Tree (Val_Node)
                                   and then KIND (Val_Node) not in DN_AGGREGATE | DN_STRING_LITERAL;
                              begin
                                 if ETI.Kind = TK_Constrained_Array and then ETI.NDims > 1
                                   and then not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_AGGREGATE
                                   and then Comp_Agg_All_Named (Val_Node, ETI.NDims)
                                 then
                                    -- LRM 4.3.2(8) (c43208b): evaluate multi-D component aggregate's choices ONCE via Pass-1!
                                    declare
                                       Base_ET : constant TREE := Subst_Base_Type (Ctx, DTI.Elem_Type);
                                       SV      : constant Expr_Val := Emit_Expr (Ctx, Val_Node, Base_ET);
                                    begin
                                       if Ctx.Enable_Checks then
                                          declare
                                             SV_W   : constant Array_View := Resolve_Array_View (Ctx, SV);
                                             DF, DL : Str_Array_4;
                                             ND_E   : constant Natural := Natural'Min (ETI.NDims, Max_Array_Dims);
                                          begin
                                             for D_I in 1 .. ND_E loop
                                                Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, DF (D_I), DL (D_I));
                                             end loop;
                                             Emit_Exact_Bounds_Check (Ctx, SV_W.Firsts, SV_W.Lasts, DF, DL, ND_E);
                                          end;
                                       end if;
                                       Emit_Copy_Composite (Ctx, EP2, SV, DTI.Elem_Type);
                                    end;
                                 else
                                    Emit_Init_Composite (Ctx, EP2, DTI.Elem_Type, Val_Node, Exact_Bounds => Need_Exact);
                                    if ETI.Kind = TK_Record then
                                       Emit_Discriminant_Check (Ctx, EP2, DTI.Elem_Type, Is_Access => False);
                                    end if;
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                  end Store_Elem_At;
               begin
                  Get_Dim_Lo_Hi (Dim, Lo_Reg, Hi_Reg);

                  -- First pass: handle any "others => val" along this dimension
                  -- Skip indices covered by explicit positional/named choices so `val` is evaluated ONLY for uncovered slots (LRM 4.3.2 / c43210a)
                  declare
                     type Static_Span is record
                        Lo, Hi : Integer_64 := 0;
                     end record;
                     Spans      : array (1 .. 64) of Static_Span;
                     Num_Spans  : Natural := 0;
                     Pos_Cnt    : Integer_64 := 0;
                     Has_Dyn_Ch : Boolean := False;
                     Scan       : SEQ_TYPE := Assocs;
                     SIt        : TREE;
                     Others_Exp : TREE := CONST_VOID;
                  begin
                     while not IS_EMPTY (Scan) loop
                        POP (Scan, SIt);
                        if KIND (SIt) = DN_ASSOC or else KIND (SIt) = DN_NAMED then
                           declare
                              CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, SIt));
                              Ch : TREE;
                           begin
                              while not IS_EMPTY (CS) loop
                                 POP (CS, Ch);
                                 if KIND (Ch) = DN_CHOICE_OTHERS then
                                    Others_Exp := D (AS_EXP, SIt);
                                 elsif KIND (Ch) = DN_CHOICE_EXP then
                                    declare
                                       SV : constant TREE := Safe_SM_Value (D (AS_EXP, Ch));
                                    begin
                                       if Has_Static_Val (SV) and then Num_Spans < 64 then
                                          Num_Spans := Num_Spans + 1;
                                          Spans (Num_Spans) := (Lo => Eval_Int64 (SV), Hi => Eval_Int64 (SV));
                                       else
                                          Has_Dyn_Ch := True;
                                       end if;
                                    end;
                                 elsif KIND (Ch) = DN_CHOICE_RANGE then
                                    declare
                                       DR : constant TREE := D (AS_DISCRETE_RANGE, Ch);
                                    begin
                                       if not Is_Void_Tree (DR) and then KIND (DR) = DN_RANGE
                                         and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, DR)))
                                         and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, DR)))
                                         and then Num_Spans < 64
                                       then
                                          Num_Spans := Num_Spans + 1;
                                          Spans (Num_Spans) := (Lo => Eval_Int64 (Safe_SM_Value (D (AS_EXP1, DR))),
                                                                Hi => Eval_Int64 (Safe_SM_Value (D (AS_EXP2, DR))));
                                       elsif not Is_Void_Tree (DR) and then KIND (DR) = DN_DISCRETE_SUBTYPE
                                         and then Num_Spans < 64
                                       then
                                          declare
                                             Sub_N : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                                             Con_N : constant TREE := (if not Is_Void_Tree (Sub_N) then D (AS_CONSTRAINT, Sub_N) else CONST_VOID);
                                             TM    : constant TREE := (if not Is_Void_Tree (Sub_N) then Get_Name_Defn (D (AS_NAME, Sub_N)) else CONST_VOID);
                                             Got_S : Boolean := False;
                                             SLo, SHi : Integer_64 := 0;
                                          begin
                                             if not Is_Void_Tree (Con_N) and then KIND (Con_N) = DN_RANGE
                                               and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, Con_N)))
                                               and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, Con_N)))
                                             then
                                                Got_S := True;
                                                SLo   := Eval_Int64 (Safe_SM_Value (D (AS_EXP1, Con_N)));
                                                SHi   := Eval_Int64 (Safe_SM_Value (D (AS_EXP2, Con_N)));
                                             elsif Is_Void_Tree (Con_N) and then not Is_Void_Tree (TM) and then KIND (TM) in CLASS_TYPE_NAME then
                                                Get_Discrete_Bounds (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TM)), Got_S, SLo, SHi);
                                             end if;
                                             if Got_S then
                                                Num_Spans := Num_Spans + 1;
                                                Spans (Num_Spans) := (Lo => SLo, Hi => SHi);
                                             else
                                                Has_Dyn_Ch := True;
                                             end if;
                                          end;
                                       else
                                          Has_Dyn_Ch := True;
                                       end if;
                                    end;
                                 end if;
                              end loop;
                           end;
                        else
                           Pos_Cnt := Pos_Cnt + 1;
                        end if;
                     end loop;

                     -- P3-3.2: Always check Pos_Cnt <= DLen when OTHERS is present (prevents buffer overflow on excess positional elements / null arrays!)
                     if Ctx.Enable_Checks and then not Is_Void_Tree (Others_Exp) and then Pos_Cnt > 0 then
                        declare
                           DLen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Lo_Reg), To_String (Hi_Reg));
                           Ovf  : constant String := Next_Reg (Ctx.E, "apovf");
                        begin
                           Emit_Inst (Ctx.E, Ovf & " = cmp_gt_s.i64 " & Img (Long_Long_Integer (Pos_Cnt)) & ", " & DLen);
                           Emit_Check_Fail_If (Ctx, Ovf);
                        end;
                     end if;
                     if not Is_Void_Tree (Others_Exp) then
                        declare
                           Tot64  : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Lo_Reg), To_String (Hi_Reg));
                           L_Hdr  : constant String := Next_Label (Ctx.E, "agg_hdr");
                           L_Body : constant String := Next_Label (Ctx.E, "agg_bdy");
                           L_Step : constant String := Next_Label (Ctx.E, "agg_stp");
                           L_Done : constant String := Next_Label (Ctx.E, "agg_end");
                           I_Reg  : constant String := Next_Reg (Ctx.E, "ai");
                           Cond   : constant String := Next_Reg (Ctx.E, "ac");
                           Next_I : constant String := Next_Reg (Ctx.E, "ain");
                           Start64 : constant String :=
                             (if Pos_Cnt > 0 and then Num_Spans = 0 and then not Has_Dyn_Ch
                              then Img (Long_Long_Integer (Pos_Cnt)) else "0");
                        begin
                           Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Start64 & ")");
                           Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                           Emit_Inst (Ctx.E, Cond & " = cmp_lt_s.i64 " & I_Reg & ", " & Tot64);
                           Emit_Term (Ctx.E, "br " & Cond & ", " & L_Body & ", " & L_Done);
                           Emit_Label (Ctx.E, L_Body);
                           if Num_Spans > 0 and then not Has_Dyn_Ch then
                              -- Compute actual Ada index Idx64 = Lo_Reg + I_Reg and skip if covered by any static span
                              declare
                                 Idx64 : constant String := Next_Reg (Ctx.E, "oidx");
                                 Cov   : Unbounded_String;
                                 L_Do  : constant String := Next_Label (Ctx.E, "agg_do");
                              begin
                                 Emit_Inst (Ctx.E, Idx64 & " = add.i64 " & To_String (Lo_Reg) & ", " & I_Reg);
                                 for S_I in 1 .. Num_Spans loop
                                    declare
                                       C1 : constant String := Next_Reg (Ctx.E, "oc1");
                                       C2 : constant String := Next_Reg (Ctx.E, "oc2");
                                       InS : constant String := Next_Reg (Ctx.E, "oin");
                                    begin
                                       Emit_Inst (Ctx.E, C1 & " = cmp_ge_s.i64 " & Idx64 & ", " & Img (Long_Long_Integer (Spans (S_I).Lo)));
                                       Emit_Inst (Ctx.E, C2 & " = cmp_le_s.i64 " & Idx64 & ", " & Img (Long_Long_Integer (Spans (S_I).Hi)));
                                       Emit_Inst (Ctx.E, InS & " = and.i8 " & C1 & ", " & C2);
                                       if S_I = 1 then
                                          Cov := To_Unbounded_String (InS);
                                       else
                                          declare
                                             OrR : constant String := Next_Reg (Ctx.E, "oor");
                                          begin
                                             Emit_Inst (Ctx.E, OrR & " = or.i8 " & To_String (Cov) & ", " & InS);
                                             Cov := To_Unbounded_String (OrR);
                                          end;
                                       end if;
                                    end;
                                 end loop;
                                 Emit_Term (Ctx.E, "br " & To_String (Cov) & ", " & L_Step & ", " & L_Do);
                                 Emit_Label (Ctx.E, L_Do);
                              end;
                           end if;
                           Store_Elem_At (I_Reg, Others_Exp);
                           Emit_Term (Ctx.E, "jmp " & L_Step);
                           Emit_Label (Ctx.E, L_Step);
                           Emit_Inst (Ctx.E, Next_I & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                           Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Next_I & ")");
                           Emit_Label (Ctx.E, L_Done);
                        end;
                     end if;
                     -- Check positional element count against dimension length when no OTHERS (c48009e, c48009f)
                     if Ctx.Enable_Checks and then Is_Void_Tree (Others_Exp)
                       and then Pos_Cnt > 0 and then Num_Spans = 0 and then not Has_Dyn_Ch
                     then
                        declare
                           DLen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Lo_Reg), To_String (Hi_Reg));
                           Mis  : constant String := Next_Reg (Ctx.E, "apmis");
                        begin
                           Emit_Inst (Ctx.E, Mis & " = cmp_ne.i64 " & DLen & ", " & Img (Long_Long_Integer (Pos_Cnt)));
                           Emit_Check_Fail_If (Ctx, Mis);
                        end;
                        -- LRM 4.3.2(9) (c43212a Case 1A): in unconstrained multi-D aggregate, a positional subaggregate
                        -- has lower bound Base_Idx'FIRST, which must match Lo_Reg from any named sibling subaggregate!
                        if DTI.Kind = TK_Unconstrained_Array and then Dim > 1
                          and then Dim <= DTI.NDims and then not Is_Void_Tree (DTI.Dims (Dim).Idx_Type)
                        then
                           declare
                              BF, BL : Unbounded_String;
                              BMis   : constant String := Next_Reg (Ctx.E, "aplo");
                           begin
                              Emit_Index_Bounds (Ctx, DTI.Dims (Dim).Idx_Type, BF, BL);
                              Emit_Inst (Ctx.E, BMis & " = cmp_ne.i64 " & To_String (Lo_Reg) & ", " & To_String (BF));
                              Emit_Check_Fail_If (Ctx, BMis);
                           end;
                        end if;
                     end if;
                  end;

                  -- Second pass: positional and named explicit elements
                  declare
                     Has_Named_R : Boolean := False;
                     Has_Others2 : Boolean := False;
                     Min_C64, Max_C64 : Unbounded_String;
                  begin
                  while not IS_EMPTY (Assocs) loop
                     POP (Assocs, Item);
                     if KIND (Item) = DN_ASSOC or else KIND (Item) = DN_NAMED then
                        declare
                           CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, Item));
                           Ch : TREE;
                        begin
                           while not IS_EMPTY (CS) loop
                              POP (CS, Ch);
                              if KIND (Ch) = DN_CHOICE_OTHERS then
                                 Has_Others2 := True;
                              elsif KIND (Ch) = DN_CHOICE_EXP then
                                 declare
                                    CE   : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch));
                                    CE_U : constant Boolean := Is_Unsigned_Scalar (CE.Ada_Type, To_String (CE.Pir_Type));
                                    C64  : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i64", Is_Unsigned => CE_U);
                                    O64  : constant String := Next_Reg (Ctx.E, "aoff64");
                                 begin
                                    if not Has_Named_R then
                                       Min_C64 := To_Unbounded_String (C64);
                                       Max_C64 := To_Unbounded_String (C64);
                                       Has_Named_R := True;
                                    else
                                       declare
                                          CL : constant String := Next_Reg (Ctx.E, "acl");
                                          ML : constant String := Next_Reg (Ctx.E, "aml");
                                          CH : constant String := Next_Reg (Ctx.E, "ach");
                                          MH : constant String := Next_Reg (Ctx.E, "amh");
                                       begin
                                          Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i64 " & C64 & ", " & To_String (Min_C64));
                                          Emit_Inst (Ctx.E, ML & " = select.i64 " & CL & ", " & C64 & ", " & To_String (Min_C64));
                                          Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i64 " & C64 & ", " & To_String (Max_C64));
                                          Emit_Inst (Ctx.E, MH & " = select.i64 " & CH & ", " & C64 & ", " & To_String (Max_C64));
                                          Min_C64 := To_Unbounded_String (ML);
                                          Max_C64 := To_Unbounded_String (MH);
                                       end;
                                    end if;
                                    if Ctx.Enable_Checks then
                                       -- Check choice lies in BASE array index subtype (c43211a) AND within destination buffer [Lo_Reg .. Hi_Reg]
                                       declare
                                          BTI_C : constant Type_Info := Get_Type_Info (Ctx, Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type))));
                                       begin
                                          if Dim <= BTI_C.NDims and then not Is_Void_Tree (BTI_C.Dims (Dim).Idx_Type) then
                                             Emit_Range_Check (Ctx, C64, "i64", BTI_C.Dims (Dim).Idx_Type);
                                          end if;
                                       end;
                                       declare
                                          B1  : constant String := Next_Reg (Ctx.E, "acb1");
                                          B2  : constant String := Next_Reg (Ctx.E, "acb2");
                                          Bad : constant String := Next_Reg (Ctx.E, "acbd");
                                       begin
                                          Emit_Inst (Ctx.E, B1 & " = cmp_lt_s.i64 " & C64 & ", " & To_String (Lo_Reg));
                                          Emit_Inst (Ctx.E, B2 & " = cmp_gt_s.i64 " & C64 & ", " & To_String (Hi_Reg));
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & B1 & ", " & B2);
                                          Emit_Check_Fail_If (Ctx, Bad);
                                       end;
                                    end if;
                                    Emit_Inst (Ctx.E, O64 & " = sub.i64 " & C64 & ", " & To_String (Lo_Reg));
                                    Store_Elem_At (O64, D (AS_EXP, Item));
                                 end;
                              elsif KIND (Ch) = DN_CHOICE_RANGE then
                                 declare
                                    RT, RL, RH : Unbounded_String;
                                    L_B : constant String := Next_Label (Ctx.E, "ar_b");
                                    L_E : constant String := Next_Label (Ctx.E, "ar_e");
                                    IV  : constant String := Next_Reg (Ctx.E, "ari");
                                    L64, H64 : Unbounded_String;
                                 begin
                                    if Ctx.Saved_Type_Ranges.Contains (Tree_Key (Ch)) then
                                       declare
                                          SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (Ch));
                                       begin
                                          L64 := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global));
                                          H64 := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global));
                                          if To_String (SR.Pir_Type) /= "i64" then
                                             L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L64), To_String (SR.Pir_Type), "i64"));
                                             H64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (H64), To_String (SR.Pir_Type), "i64"));
                                          end if;
                                       end;
                                    else
                                       Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                       declare
                                          BTI_U : constant Type_Info := Get_Type_Info (Ctx, Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type))));
                                          RU    : constant Boolean :=
                                            Dim <= BTI_U.NDims and then not Is_Void_Tree (BTI_U.Dims (Dim).Idx_Type)
                                            and then Is_Unsigned_Scalar (BTI_U.Dims (Dim).Idx_Type, To_String (RT));
                                       begin
                                          L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64", Is_Unsigned => RU));
                                          H64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64", Is_Unsigned => RU));
                                       end;
                                    end if;
                                    if not Has_Named_R then
                                       Min_C64 := L64;
                                       Max_C64 := H64;
                                       Has_Named_R := True;
                                    else
                                       declare
                                          CL : constant String := Next_Reg (Ctx.E, "acl");
                                          ML : constant String := Next_Reg (Ctx.E, "aml");
                                          CH : constant String := Next_Reg (Ctx.E, "ach");
                                          MH : constant String := Next_Reg (Ctx.E, "amh");
                                       begin
                                          Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i64 " & To_String (L64) & ", " & To_String (Min_C64));
                                          Emit_Inst (Ctx.E, ML & " = select.i64 " & CL & ", " & To_String (L64) & ", " & To_String (Min_C64));
                                          Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i64 " & To_String (H64) & ", " & To_String (Max_C64));
                                          Emit_Inst (Ctx.E, MH & " = select.i64 " & CH & ", " & To_String (H64) & ", " & To_String (Max_C64));
                                          Min_C64 := To_Unbounded_String (ML);
                                          Max_C64 := To_Unbounded_String (MH);
                                       end;
                                    end if;
                                    if Ctx.Enable_Checks then
                                       -- Non-null choice range must lie in BASE index subtype (c43211a) and within [Lo_Reg .. Hi_Reg] (c43206a)
                                       declare
                                          NE    : constant String := Next_Reg (Ctx.E, "arne");
                                          L_Chk : constant String := Next_Label (Ctx.E, "arc");
                                          L_Ok  : constant String := Next_Label (Ctx.E, "aro");
                                          B1    : constant String := Next_Reg (Ctx.E, "arb1");
                                          B2    : constant String := Next_Reg (Ctx.E, "arb2");
                                          Bad   : constant String := Next_Reg (Ctx.E, "arbd");
                                          BTI_C : constant Type_Info := Get_Type_Info (Ctx, Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type))));
                                       begin
                                          Emit_Inst (Ctx.E, NE & " = cmp_le_s.i64 " & To_String (L64) & ", " & To_String (H64));
                                          Emit_Term (Ctx.E, "br " & NE & ", " & L_Chk & ", " & L_Ok);
                                          Emit_Label (Ctx.E, L_Chk);
                                          if Dim <= BTI_C.NDims and then not Is_Void_Tree (BTI_C.Dims (Dim).Idx_Type) then
                                             Emit_Range_Check (Ctx, To_String (L64), "i64", BTI_C.Dims (Dim).Idx_Type);
                                             Emit_Range_Check (Ctx, To_String (H64), "i64", BTI_C.Dims (Dim).Idx_Type);
                                          end if;
                                          Emit_Inst (Ctx.E, B1 & " = cmp_lt_s.i64 " & To_String (L64) & ", " & To_String (Lo_Reg));
                                          Emit_Inst (Ctx.E, B2 & " = cmp_gt_s.i64 " & To_String (H64) & ", " & To_String (Hi_Reg));
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & B1 & ", " & B2);
                                          Emit_Check_Fail_If (Ctx, Bad);
                                          Emit_Term (Ctx.E, "jmp " & L_Ok);
                                          Emit_Label (Ctx.E, L_Ok);
                                       end;
                                    end if;
                                    declare
                                       Empty_C : constant String := Next_Reg (Ctx.E, "are");
                                       Done_C  : constant String := Next_Reg (Ctx.E, "ard");
                                       L_S     : constant String := Next_Label (Ctx.E, "ars");
                                    begin
                                       Emit_Inst (Ctx.E, Empty_C & " = cmp_gt_s.i64 " & To_String (L64) & ", " & To_String (H64));
                                       Emit_Term (Ctx.E, "br " & Empty_C & ", " & L_E & ", " & L_B & "(" & To_String (L64) & ")");
                                       Emit_Label (Ctx.E, L_B, IV & ": i64");
                                       declare
                                          O64 : constant String := Next_Reg (Ctx.E, "aro64");
                                          Nx  : constant String := Next_Reg (Ctx.E, "arn");
                                       begin
                                          Emit_Inst (Ctx.E, O64 & " = sub.i64 " & IV & ", " & To_String (Lo_Reg));
                                          Store_Elem_At (O64, D (AS_EXP, Item));
                                          Emit_Inst (Ctx.E, Done_C & " = cmp_eq.i64 " & IV & ", " & To_String (H64));
                                          Emit_Term (Ctx.E, "br " & Done_C & ", " & L_E & ", " & L_S);
                                          Emit_Label (Ctx.E, L_S);
                                          Emit_Inst (Ctx.E, Nx & " = add.i64 " & IV & ", 1");
                                          Emit_Term (Ctx.E, "jmp " & L_B & "(" & Nx & ")");
                                       end;
                                       Emit_Label (Ctx.E, L_E);
                                    end;
                                 end;
                              end if;
                           end loop;
                        end;
                     else
                        -- Positional element
                        Store_Elem_At (Img (Long_Long_Integer (Pos)), Item);
                        Pos := Pos + 1;
                     end if;
                  end loop;
                  -- In qualified/exact-bounds or multidimensional subaggregate (Dim > 1), named bounds MUST match [Lo_Reg .. Hi_Reg] (c43212a/c, c48009e/f);
                  -- otherwise named aggregate without OTHERS must at least match dimension length (c48009f CASE 2)!
                  if Ctx.Enable_Checks and then Has_Named_R and then not Has_Others2 then
                     if Exact_Bounds or else Dim > 1 then
                        declare
                           E1  : constant String := Next_Reg (Ctx.E, "aeb1");
                           E2  : constant String := Next_Reg (Ctx.E, "aeb2");
                           Bad : constant String := Next_Reg (Ctx.E, "aebb");
                        begin
                           Emit_Inst (Ctx.E, E1 & " = cmp_ne.i64 " & To_String (Min_C64) & ", " & To_String (Lo_Reg));
                           Emit_Inst (Ctx.E, E2 & " = cmp_ne.i64 " & To_String (Max_C64) & ", " & To_String (Hi_Reg));
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & E1 & ", " & E2);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end;
                     else
                        declare
                           ALen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Min_C64), To_String (Max_C64));
                           DLen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Lo_Reg), To_String (Hi_Reg));
                           Mis  : constant String := Next_Reg (Ctx.E, "anmis");
                        begin
                           Emit_Inst (Ctx.E, Mis & " = cmp_ne.i64 " & ALen & ", " & DLen);
                           Emit_Check_Fail_If (Ctx, Mis);
                        end;
                     end if;
                  end if;
                  end;
               end Emit_Init_Array_Dim;
            begin
               Emit_Init_Array_Dim (Dst_Ptr, Init_Node, 1);
            end;
            return;
         else
            return;
         end if;
      end if;

      -- General composite expression copy
      declare
         Src : constant Expr_Val := Emit_Expr (Ctx, Init_Node, Dst_Type);
      begin
         if Ctx.Enable_Checks and then DTI.Kind = TK_Constrained_Array then
            declare
               ND_A : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
               Tgt  : Expr_Val;
            begin
               Tgt.Kind := EK_Slice; Tgt.Ada_Type := Dst_Type; Tgt.NDims := ND_A;
               for D_I in 1 .. ND_A loop
                  if Length (Dst_Firsts (D_I)) > 0 then
                     Tgt.First_Regs (D_I) := Dst_Firsts (D_I);
                     Tgt.Last_Regs (D_I)  := Dst_Lasts (D_I);
                  else
                     Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                  end if;
               end loop;
               if Exact_Bounds then
                  declare
                     SV_W : constant Array_View := Resolve_Array_View (Ctx, Src);
                  begin
                     Emit_Exact_Bounds_Check (Ctx, SV_W.Firsts, SV_W.Lasts, Tgt.First_Regs, Tgt.Last_Regs, ND_A);
                  end;
               else
                  Emit_Array_Length_Check (Ctx, Tgt, Src, ND_A);
               end if;
            end;
         end if;
         Emit_Copy_Composite (Ctx, Dst_Ptr, Src, Dst_Type);
      end;
   end Emit_Init_Composite;

   -- P3-6.2 / P4: Resolve the most-derived DN_ENUMERATION carrying custom SM_REP codes
   -- (walking SM_BASE_TYPE without jumping over derived types' own overriding rep clauses,
   -- then walking SM_DERIVED only when no explicit SM_REP exists on the derived type).
   function Find_Enum_Rep_Type (Ctx : Context; T : TREE) return TREE is
      Cur : TREE := Base_Of_Type (Ctx, T);
      LS  : TREE;
      EL  : SEQ_TYPE;
      Lit : TREE;
   begin
      for Step in 1 .. 16 loop
         exit when Is_Void_Tree (Cur) or else KIND (Cur) /= DN_ENUMERATION;
         LS := D (SM_LITERAL_S, Cur);
         if not Is_Void_Tree (LS) then
            declare
               Any_Rep   : Boolean := False;
               Non_Ident : Boolean := False;
            begin
               EL := LIST (LS);
               while not IS_EMPTY (EL) loop
                  POP (EL, Lit);
                  if Has_Static_Val (D (SM_REP, Lit)) then
                     Any_Rep := True;
                     if Eval_Int64 (D (SM_REP, Lit)) /= Integer_64 (DI (SM_POS, Lit)) then
                        Non_Ident := True;
                     end if;
                  end if;
               end loop;
               if Non_Ident then
                  return Cur;
               elsif Any_Rep then
                  -- Explicit rep clause on this derived type overrides parent back to identity
                  return CONST_VOID;
               end if;
            end;
         end if;
         declare
            Next_E : constant TREE := Subst_Resolve_Type (Ctx, D (SM_DERIVED, Cur));
         begin
            exit when Is_Void_Tree (Next_E) or else Next_E = Cur;
            Cur := Next_E;
         end;
      end loop;
      return CONST_VOID;
   end Find_Enum_Rep_Type;

   function Enum_Rep_Has_Negative (BT : TREE) return Boolean is
      LS  : TREE;
      EL  : SEQ_TYPE;
      Lit : TREE;
   begin
      if Is_Void_Tree (BT) or else KIND (BT) /= DN_ENUMERATION then
         return False;
      end if;
      LS := D (SM_LITERAL_S, BT);
      if Is_Void_Tree (LS) then return False; end if;
      EL := LIST (LS);
      while not IS_EMPTY (EL) loop
         POP (EL, Lit);
         if Has_Static_Val (D (SM_REP, Lit)) and then Eval_Int64 (D (SM_REP, Lit)) < 0 then
            return True;
         end if;
      end loop;
      return False;
   end Enum_Rep_Has_Negative;

   function Emit_Enum_Rep_Table (Ctx : in out Context; BT : TREE; Count : out Natural) return String is
      Sym : constant String := "@ada_erep_" & Tree_Key_Str (BT);
      EL  : SEQ_TYPE := LIST (D (SM_LITERAL_S, BT));
      Lit : TREE;
      Buf : Unbounded_String;
      N   : Natural := 0;
   begin
      while not IS_EMPTY (EL) loop
         POP (EL, Lit);
         declare
            RV : constant TREE := D (SM_REP, Lit);
            IV : constant Integer_64 :=
              (if Has_Static_Val (RV) then Eval_Int64 (RV)
               else Integer_64 (DI (SM_POS, Lit)));
            UV : Unsigned_64 :=
              (if IV >= 0 then Unsigned_64 (IV)
               else Unsigned_64'Mod (IV));
         begin
            for Bk in 0 .. 7 loop
               if N > 0 or else Bk > 0 then Append (Buf, ", "); end if;
               Append (Buf, Img (Natural (UV and 16#FF#)));
               UV := Shift_Right (UV, 8);
            end loop;
            N := N + 1;
         end;
      end loop;
      Count := N;
      Add_Data_Decl (Ctx.E, Sym, Sym & " = data align 8 { " & To_String (Buf) & " }");
      return Sym;
   end Emit_Enum_Rep_Table;

   function Emit_Enum_Pos_To_Rep_I64
     (Ctx    : in out Context;
      BT     : TREE;
      Val_R  : String;
      Pir_Ty : String) return String
   is
      Cnt : Natural := 0;
      Tbl : constant String := Emit_Enum_Rep_Table (Ctx, BT, Cnt);
      P64 : constant String := Emit_Int_Convert (Ctx, Val_R, Pir_Ty, "i64", Is_Unsigned => True);
      TA  : constant String := Next_Reg (Ctx.E, "erpt");
      Off : constant String := Next_Reg (Ctx.E, "erpo");
      EP  : constant String := Next_Reg (Ctx.E, "erpp");
      RV  : constant String := Next_Reg (Ctx.E, "erpv");
   begin
      pragma Unreferenced (Cnt);
      Emit_Inst (Ctx.E, TA & " = addr " & Tbl);
      Emit_Inst (Ctx.E, Off & " = mul.i64 " & P64 & ", 8 [unsafe_s]");
      Emit_Inst (Ctx.E, EP & " = ptr_add.i64 " & TA & ", " & Off & " [inbounds]");
      Emit_Inst (Ctx.E, RV & " = load.i64 " & EP & ", align 8");
      return RV;
   end Emit_Enum_Pos_To_Rep_I64;

   function Emit_Enum_Rep_I64_To_Pos
     (Ctx     : in out Context;
      BT      : TREE;
      Rep_I64 : String) return String
   is
      Cnt  : Natural := 0;
      Tbl  : constant String := Emit_Enum_Rep_Table (Ctx, BT, Cnt);
      TA   : constant String := Next_Reg (Ctx.E, "er2t");
      L_H  : constant String := Next_Label (Ctx.E, "er2_h");
      L_B  : constant String := Next_Label (Ctx.E, "er2_b");
      L_N  : constant String := Next_Label (Ctx.E, "er2_n");
      L_D  : constant String := Next_Label (Ctx.E, "er2_d");
      IR   : constant String := Next_Reg (Ctx.E, "er2i");
      Cnd  : constant String := Next_Reg (Ctx.E, "er2c");
      Off  : constant String := Next_Reg (Ctx.E, "er2o");
      EP   : constant String := Next_Reg (Ctx.E, "er2p");
      CV   : constant String := Next_Reg (Ctx.E, "er2v");
      Eq   : constant String := Next_Reg (Ctx.E, "er2e");
      Nx   : constant String := Next_Reg (Ctx.E, "er2x");
      PosR : constant String := Next_Reg (Ctx.E, "er2r");
   begin
      Emit_Inst (Ctx.E, TA & " = addr " & Tbl);
      Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
      Emit_Label (Ctx.E, L_H, IR & ": i64");
      Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Img (Cnt));
      Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_D & "(0)");
      Emit_Label (Ctx.E, L_B);
      Emit_Inst (Ctx.E, Off & " = mul.i64 " & IR & ", 8 [unsafe_s]");
      Emit_Inst (Ctx.E, EP & " = ptr_add.i64 " & TA & ", " & Off & " [inbounds]");
      Emit_Inst (Ctx.E, CV & " = load.i64 " & EP & ", align 8");
      Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & CV & ", " & Rep_I64);
      Emit_Term (Ctx.E, "br " & Eq & ", " & L_D & "(" & IR & "), " & L_N);
      Emit_Label (Ctx.E, L_N);
      Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
      Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
      Emit_Label (Ctx.E, L_D, PosR & ": i64");
      return PosR;
   end Emit_Enum_Rep_I64_To_Pos;

   -- P3-6.3: Shared variant discriminant check for record component access (including bitfield load/store/out!)
   procedure Emit_Variant_Check_For_Field
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_T     : TREE;
      Field_Def : TREE;
      Use_CK    : Unsigned_32)
   is
      Base_R     : constant TREE := Subst_Base_Type (Ctx, Rec_T);
      RTI        : constant Type_Info := Get_Type_Info (Ctx, Base_R);
      Target_Def : constant TREE := Canonical_Def (Field_Def);

      function Comp_List_Contains (CL : TREE) return Boolean is
      begin
         if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return False; end if;
         if not Is_Void_Tree (D (AS_DECL_S, CL)) then
            declare
               DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
               DI : TREE;
            begin
               while not IS_EMPTY (DL) loop
                  POP (DL, DI);
                  if KIND (DI) = DN_VARIABLE_DECL then
                     declare
                        NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DI));
                        ID : TREE;
                     begin
                        while not IS_EMPTY (NS) loop
                           POP (NS, ID);
                           if Canonical_Def (ID) = Target_Def or else Tree_Key (ID) = Use_CK then
                              return True;
                           end if;
                        end loop;
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
                     if KIND (VN) = DN_VARIANT and then Comp_List_Contains (D (AS_COMP_LIST, VN)) then
                        return True;
                     end if;
                  end loop;
               end;
            end if;
         end;
         return False;
      end Comp_List_Contains;

      procedure Check_Variant_Chain (CL : TREE) is
         VP : TREE;
      begin
         if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return; end if;
         VP := D (AS_VARIANT_PART, CL);
         if Is_Void_Tree (VP) or else KIND (VP) /= DN_VARIANT_PART or else Is_Void_Tree (D (AS_VARIANT_S, VP)) then
            return;
         end if;
         declare
            D_Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, VP))));
            DK    : constant Unsigned_32 := Tree_Key (D_Def);
         begin
            if not Ctx.Fields.Contains (DK) then return; end if;
            declare
               DFE   : constant Field_Entry := Ctx.Fields.Element (DK);
               DT_A  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, D_Def));
               DTI_D : constant Type_Info := Get_Type_Info (Ctx, DT_A);
               DV64  : constant String := Emit_Load_Field_I64 (Ctx, Rec_Ptr, To_String (RTI.Pir_Type), DFE, DT_A, DTI_D);
               VL2   : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
               VN2   : TREE;
               Matched_V : TREE := CONST_VOID;
               Is_Others_V : Boolean := False;
               Other_Cond : Unbounded_String;
            begin
               while not IS_EMPTY (VL2) loop
                  POP (VL2, VN2);
                  if KIND (VN2) = DN_VARIANT then
                     if Comp_List_Contains (D (AS_COMP_LIST, VN2)) then
                        Matched_V := VN2;
                        declare
                           CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN2));
                           Ch : TREE;
                        begin
                           while not IS_EMPTY (CS) loop
                              POP (CS, Ch);
                              if KIND (Ch) = DN_CHOICE_OTHERS then Is_Others_V := True; end if;
                           end loop;
                        end;
                     else
                        declare
                           Dummy_Ot : Boolean := False;
                           OC : constant String := Emit_Variant_Choice_Match (Ctx, DV64, DT_A, D (AS_CHOICE_S, VN2), Dummy_Ot);
                        begin
                           pragma Unreferenced (Dummy_Ot);
                           if OC'Length > 0 then
                              if Length (Other_Cond) = 0 then Other_Cond := To_Unbounded_String (OC);
                              else
                                 declare
                                    O2 : constant String := Next_Reg (Ctx.E, "vcoo");
                                 begin
                                    Emit_Inst (Ctx.E, O2 & " = or.i8 " & To_String (Other_Cond) & ", " & OC);
                                    Other_Cond := To_Unbounded_String (O2);
                                 end;
                              end if;
                           end if;
                        end;
                     end if;
                  end if;
               end loop;
               if not Is_Void_Tree (Matched_V) then
                  if Is_Others_V then
                     if Length (Other_Cond) > 0 then
                        Emit_Check_Fail_If (Ctx, To_String (Other_Cond));
                     end if;
                  else
                     declare
                        Dummy_Ot : Boolean := False;
                        MC : constant String := Emit_Variant_Choice_Match (Ctx, DV64, DT_A, D (AS_CHOICE_S, Matched_V), Dummy_Ot);
                     begin
                        pragma Unreferenced (Dummy_Ot);
                        if MC'Length > 0 then
                           declare
                              Bad : constant String := Next_Reg (Ctx.E, "vcbd");
                           begin
                              Emit_Inst (Ctx.E, Bad & " = xor.i8 " & MC & ", 1");
                              Emit_Check_Fail_If (Ctx, Bad);
                           end;
                        end if;
                     end;
                  end if;
                  Check_Variant_Chain (D (AS_COMP_LIST, Matched_V));
               end if;
            end;
         end;
      end Check_Variant_Chain;
   begin
      if not Ctx.Enable_Checks then return; end if;
      if not Is_Void_Tree (Base_R) and then KIND (Base_R) = DN_RECORD then
         Check_Variant_Chain (D (SM_COMP_LIST, Base_R));
      end if;
   end Emit_Variant_Check_For_Field;

   function Emit_Bitfield_Load
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      FE        : Field_Entry;
      Comp_Type : TREE) return String
   is
      TI          : constant Type_Info := Get_Type_Info (Ctx, Comp_Type);
      BT_Comp     : constant TREE := Find_Enum_Rep_Type (Ctx, Comp_Type);
      Has_ERep    : constant Boolean := not Is_Void_Tree (BT_Comp);
      Total_Bytes : constant Natural := (FE.Bit_Offset + FE.Bit_Size + 7) / 8;
      Low_Bytes   : constant Natural := Natural'Min (8, Total_Bytes);
      Acc         : Unbounded_String;
      Sh_R        : Unbounded_String;
      Masked      : Unbounded_String;
      Is_Sig      : Boolean := False;
   begin
      if Is_Integer_Type (Comp_Type) then
         declare
            St     : Boolean := False;
            Lo, Hi : Integer_64 := 0;
         begin
            Get_Discrete_Bounds (Comp_Type, St, Lo, Hi);
            Is_Sig := (not St) or else Lo < 0;
         end;
      elsif Is_Fixed_Type (Comp_Type) then
         -- P3-6.1: Sign-extend fixed-point bitfields ONLY when the subtype's lower bound is negative!
         declare
            RT_F : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Comp_Type));
            Rng  : constant TREE := (if not Is_Void_Tree (RT_F) and then KIND (RT_F) = DN_FIXED then D (SM_RANGE, RT_F) else CONST_VOID);
         begin
            if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE
              and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, Rng)))
            then
               Is_Sig := Eval_Float64 (Safe_SM_Value (D (AS_EXP1, Rng))) < 0.0;
            else
               Is_Sig := True;
            end if;
         end;
      elsif Has_ERep then
         -- P3-6.2: Sign-extend enum bitfield if its SM_REP codes include negative values!
         Is_Sig := Enum_Rep_Has_Negative (BT_Comp);
      end if;
      for K in 0 .. Low_Bytes - 1 loop
         declare
            Off_R : constant String := Next_Reg (Ctx.E, "bfo");
            BP    : constant String := Next_Reg (Ctx.E, "bfp");
            BV    : constant String := Next_Reg (Ctx.E, "bfv");
            B64   : constant String := Next_Reg (Ctx.E, "bf6");
            ShV   : Unbounded_String := To_Unbounded_String (B64);
         begin
            Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset + K));
            Emit_Inst (Ctx.E, BP & " = ptr_add.i64 " & Rec_Ptr & ", " & Off_R & " [inbounds]");
            Emit_Inst (Ctx.E, BV & " = load.i8 " & BP & ", align 1");
            Emit_Inst (Ctx.E, B64 & " = zext.i64 " & BV);
            if K > 0 then
               declare
                  SR : constant String := Next_Reg (Ctx.E, "bfs");
               begin
                  Emit_Inst (Ctx.E, SR & " = shl.i64 " & B64 & ", " & Img (K * 8));
                  ShV := To_Unbounded_String (SR);
               end;
            end if;
            if K = 0 then
               Acc := ShV;
            else
               declare
                  OR_R : constant String := Next_Reg (Ctx.E, "bfr");
               begin
                  Emit_Inst (Ctx.E, OR_R & " = or.i64 " & To_String (Acc) & ", " & To_String (ShV));
                  Acc := To_Unbounded_String (OR_R);
               end;
            end if;
         end;
      end loop;
      if FE.Bit_Offset > 0 then
         declare
            SR : constant String := Next_Reg (Ctx.E, "bfrsh");
         begin
            Emit_Inst (Ctx.E, SR & " = shr.i64 " & To_String (Acc) & ", " & Img (FE.Bit_Offset));
            Sh_R := To_Unbounded_String (SR);
         end;
         -- Handle 9th byte when Bit_Offset + Bit_Size > 64 (e.g., 64-bit bitfield at non-zero bit offset!)
         if Total_Bytes > 8 then
            declare
               Off9 : constant String := Next_Reg (Ctx.E, "bfo9");
               BP9  : constant String := Next_Reg (Ctx.E, "bfp9");
               BV9  : constant String := Next_Reg (Ctx.E, "bfv9");
               B649 : constant String := Next_Reg (Ctx.E, "bf69");
               SH9  : constant String := Next_Reg (Ctx.E, "bfs9");
               OR9  : constant String := Next_Reg (Ctx.E, "bfr9");
            begin
               Emit_Inst (Ctx.E, Off9 & " = const.i64 " & Img (FE.Byte_Offset + 8));
               Emit_Inst (Ctx.E, BP9 & " = ptr_add.i64 " & Rec_Ptr & ", " & Off9 & " [inbounds]");
               Emit_Inst (Ctx.E, BV9 & " = load.i8 " & BP9 & ", align 1");
               Emit_Inst (Ctx.E, B649 & " = zext.i64 " & BV9);
               Emit_Inst (Ctx.E, SH9 & " = shl.i64 " & B649 & ", " & Img (64 - FE.Bit_Offset));
               Emit_Inst (Ctx.E, OR9 & " = or.i64 " & To_String (Sh_R) & ", " & SH9);
               Sh_R := To_Unbounded_String (OR9);
            end;
         end if;
      else
         Sh_R := Acc;
      end if;
      if FE.Bit_Size < 64 then
         if Is_Sig then
            declare
               L_Sh : constant String := Next_Reg (Ctx.E, "bfsl");
               R_Sh : constant String := Next_Reg (Ctx.E, "bfsr");
               Rem_B : constant Natural := 64 - FE.Bit_Size;
            begin
               Emit_Inst (Ctx.E, L_Sh & " = shl.i64 " & To_String (Sh_R) & ", " & Img (Rem_B));
               Emit_Inst (Ctx.E, R_Sh & " = sar.i64 " & L_Sh & ", " & Img (Rem_B));
               Masked := To_Unbounded_String (R_Sh);
            end;
         else
            declare
               M_Val : constant Unsigned_64 := Shift_Left (Unsigned_64 (1), FE.Bit_Size) - 1;
               MR    : constant String := Next_Reg (Ctx.E, "bfm");
               AR    : constant String := Next_Reg (Ctx.E, "bfa");
            begin
               Emit_Inst (Ctx.E, MR & " = const.i64 " & Img (Long_Long_Integer (M_Val)));
               Emit_Inst (Ctx.E, AR & " = and.i64 " & To_String (Sh_R) & ", " & MR);
               Masked := To_Unbounded_String (AR);
            end;
         end if;
      else
         Masked := Sh_R;
      end if;
      if Has_ERep then
         -- P3-6.2: Map extracted representation code back to enumeration position!
         declare
            Pos64 : constant String := Emit_Enum_Rep_I64_To_Pos (Ctx, BT_Comp, To_String (Masked));
         begin
            return Emit_Int_Convert (Ctx, Pos64, "i64", To_String (TI.Pir_Type));
         end;
      elsif To_String (TI.Pir_Type) = "f32" then
         declare
            I32 : constant String := Emit_Int_Convert (Ctx, To_String (Masked), "i64", "i32");
            F32 : constant String := Next_Reg (Ctx.E, "bff32");
         begin
            Emit_Inst (Ctx.E, F32 & " = bitcast.f32 " & I32);
            return F32;
         end;
      elsif To_String (TI.Pir_Type) = "f64" then
         declare
            F64 : constant String := Next_Reg (Ctx.E, "bff64");
         begin
            Emit_Inst (Ctx.E, F64 & " = bitcast.f64 " & To_String (Masked));
            return F64;
         end;
      elsif To_String (TI.Pir_Type) = "ptr" then
         declare
            PR : constant String := Next_Reg (Ctx.E, "bfptr");
         begin
            Emit_Inst (Ctx.E, PR & " = inttoptr.ptr " & To_String (Masked));
            return PR;
         end;
      else
         return Emit_Int_Convert (Ctx, To_String (Masked), "i64", To_String (TI.Pir_Type));
      end if;
   end Emit_Bitfield_Load;

   procedure Emit_Bitfield_Store
     (Ctx          : in out Context;
      Rec_Ptr      : String;
      FE           : Field_Entry;
      Val_Reg      : String;
      Val_Pir_Type : String;
      Comp_Type    : TREE)
   is
      BT_Comp     : constant TREE := Find_Enum_Rep_Type (Ctx, Comp_Type);
      Has_ERep    : constant Boolean := not Is_Void_Tree (BT_Comp);
      Total_Bytes : constant Natural := (FE.Bit_Offset + FE.Bit_Size + 7) / 8;
      N_Bytes     : constant Natural := Natural'Min (8, Total_Bytes);
      V64         : Unbounded_String;
      Shifted     : Unbounded_String;
   begin
      if Has_ERep then
         -- P3-6.2: Map enumeration position -> representation code before packing into bitfield!
         V64 := To_Unbounded_String (Emit_Enum_Pos_To_Rep_I64 (Ctx, BT_Comp, Val_Reg, Val_Pir_Type));
      elsif Val_Pir_Type = "f32" then
         declare
            I32 : constant String := Next_Reg (Ctx.E, "bsi32");
            I64 : constant String := Next_Reg (Ctx.E, "bsi64");
         begin
            Emit_Inst (Ctx.E, I32 & " = bitcast.i32 " & Val_Reg);
            Emit_Inst (Ctx.E, I64 & " = zext.i64 " & I32);
            V64 := To_Unbounded_String (I64);
         end;
      elsif Val_Pir_Type = "f64" then
         declare
            I64 : constant String := Next_Reg (Ctx.E, "bsi64");
         begin
            Emit_Inst (Ctx.E, I64 & " = bitcast.i64 " & Val_Reg);
            V64 := To_Unbounded_String (I64);
         end;
      elsif Val_Pir_Type = "ptr" then
         declare
            I64 : constant String := Next_Reg (Ctx.E, "bsp64");
         begin
            Emit_Inst (Ctx.E, I64 & " = ptrtoint.i64 " & Val_Reg);
            V64 := To_Unbounded_String (I64);
         end;
      else
         V64 := To_Unbounded_String (Emit_Int_Convert (Ctx, Val_Reg, Val_Pir_Type, "i64",
           Is_Unsigned => Is_Unsigned_Scalar (Comp_Type, Val_Pir_Type)));
      end if;
      if FE.Bit_Size < 64 then
         declare
            M_Val : constant Unsigned_64 := Shift_Left (Unsigned_64 (1), FE.Bit_Size) - 1;
            MR    : constant String := Next_Reg (Ctx.E, "bsm");
            AR    : constant String := Next_Reg (Ctx.E, "bsa");
         begin
            Emit_Inst (Ctx.E, MR & " = const.i64 " & Img (Long_Long_Integer (M_Val)));
            Emit_Inst (Ctx.E, AR & " = and.i64 " & To_String (V64) & ", " & MR);
            V64 := To_Unbounded_String (AR);
         end;
      end if;
      if FE.Bit_Offset > 0 then
         declare
            SR : constant String := Next_Reg (Ctx.E, "bssh");
         begin
            Emit_Inst (Ctx.E, SR & " = shl.i64 " & To_String (V64) & ", " & Img (FE.Bit_Offset));
            Shifted := To_Unbounded_String (SR);
         end;
      else
         Shifted := V64;
      end if;
      for K in 0 .. N_Bytes - 1 loop
         declare
            Lo_In_Byte : constant Natural := (if K = 0 then FE.Bit_Offset else 0);
            End_Tot    : constant Natural := FE.Bit_Offset + FE.Bit_Size;
            Hi_In_Byte : constant Natural := Natural'Min (8, End_Tot - K * 8);
            Bits_Here  : constant Natural := Hi_In_Byte - Lo_In_Byte;
            Byte_Mask  : constant Natural := Natural (Shift_Left (Shift_Left (Unsigned_64 (1), Bits_Here) - 1, Lo_In_Byte));
            Off_R      : constant String := Next_Reg (Ctx.E, "bso");
            BP         : constant String := Next_Reg (Ctx.E, "bsp");
            Byte_Val   : Unbounded_String;
         begin
            Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset + K));
            Emit_Inst (Ctx.E, BP & " = ptr_add.i64 " & Rec_Ptr & ", " & Off_R & " [inbounds]");
            if K > 0 then
               declare
                  RSh : constant String := Next_Reg (Ctx.E, "bsr");
                  Tr  : constant String := Next_Reg (Ctx.E, "bst");
               begin
                  Emit_Inst (Ctx.E, RSh & " = shr.i64 " & To_String (Shifted) & ", " & Img (K * 8));
                  Emit_Inst (Ctx.E, Tr & " = trunc.i8 " & RSh);
                  Byte_Val := To_Unbounded_String (Tr);
               end;
            else
               declare
                  Tr : constant String := Next_Reg (Ctx.E, "bst");
               begin
                  Emit_Inst (Ctx.E, Tr & " = trunc.i8 " & To_String (Shifted));
                  Byte_Val := To_Unbounded_String (Tr);
               end;
            end if;
            if Byte_Mask = 255 then
               Emit_Inst (Ctx.E, "store.i8 " & To_String (Byte_Val) & ", " & BP & ", align 1");
            else
               declare
                  Old_B : constant String := Next_Reg (Ctx.E, "bsol");
                  Clr_M : constant Natural := 255 - Byte_Mask;
                  Cl_B  : constant String := Next_Reg (Ctx.E, "bscl");
                  New_B : constant String := Next_Reg (Ctx.E, "bsnw");
                  An_B  : constant String := Next_Reg (Ctx.E, "bsan");
               begin
                  Emit_Inst (Ctx.E, Old_B & " = load.i8 " & BP & ", align 1");
                  Emit_Inst (Ctx.E, Cl_B & " = and.i8 " & Old_B & ", " & Img (Clr_M));
                  Emit_Inst (Ctx.E, An_B & " = and.i8 " & To_String (Byte_Val) & ", " & Img (Byte_Mask));
                  Emit_Inst (Ctx.E, New_B & " = or.i8 " & Cl_B & ", " & An_B);
                  Emit_Inst (Ctx.E, "store.i8 " & New_B & ", " & BP & ", align 1");
               end;
            end if;
         end;
      end loop;
      -- Handle 9th byte when Bit_Offset + Bit_Size > 64 (e.g., 64-bit bitfield at non-zero bit offset!)
      if Total_Bytes > 8 and then FE.Bit_Offset > 0 then
         declare
            Bits_9    : constant Natural := (FE.Bit_Offset + FE.Bit_Size) - 64;
            Byte_Mask : constant Natural := Natural (Shift_Left (Unsigned_64 (1), Bits_9) - 1);
            Off9      : constant String := Next_Reg (Ctx.E, "bso9");
            BP9       : constant String := Next_Reg (Ctx.E, "bsp9");
            RSh9      : constant String := Next_Reg (Ctx.E, "bsr9");
            Tr9       : constant String := Next_Reg (Ctx.E, "bst9");
            Old_B     : constant String := Next_Reg (Ctx.E, "bsol9");
            Clr_M     : constant Natural := 255 - Byte_Mask;
            Cl_B      : constant String := Next_Reg (Ctx.E, "bscl9");
            An_B      : constant String := Next_Reg (Ctx.E, "bsan9");
            New_B     : constant String := Next_Reg (Ctx.E, "bsnw9");
         begin
            Emit_Inst (Ctx.E, Off9 & " = const.i64 " & Img (FE.Byte_Offset + 8));
            Emit_Inst (Ctx.E, BP9 & " = ptr_add.i64 " & Rec_Ptr & ", " & Off9 & " [inbounds]");
            Emit_Inst (Ctx.E, RSh9 & " = shr.i64 " & To_String (V64) & ", " & Img (64 - FE.Bit_Offset));
            Emit_Inst (Ctx.E, Tr9 & " = trunc.i8 " & RSh9);
            if Byte_Mask = 255 then
               Emit_Inst (Ctx.E, "store.i8 " & Tr9 & ", " & BP9 & ", align 1");
            else
               Emit_Inst (Ctx.E, Old_B & " = load.i8 " & BP9 & ", align 1");
               Emit_Inst (Ctx.E, Cl_B & " = and.i8 " & Old_B & ", " & Img (Clr_M));
               Emit_Inst (Ctx.E, An_B & " = and.i8 " & Tr9 & ", " & Img (Byte_Mask));
               Emit_Inst (Ctx.E, New_B & " = or.i8 " & Cl_B & ", " & An_B);
               Emit_Inst (Ctx.E, "store.i8 " & New_B & ", " & BP9 & ", align 1");
            end if;
         end;
      end if;
   end Emit_Bitfield_Store;

   -- P3-6.7: Field-by-field change-of-representation copy between two record buffers (LRM 13.6)
   function Emit_Field_Addr
     (Ctx     : in out Context;
      Rec_Ptr : String;
      Rec_Pir : String;
      FE      : Field_Entry;
      Prefix  : String := "fp") return String
   is
      FP : constant String := Next_Reg (Ctx.E, Prefix);
   begin
      if FE.In_Union or else FE.Has_Explicit_Offset then
         declare
            Off_R : constant String := Next_Reg (Ctx.E, Prefix & "o");
         begin
            Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
            Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Rec_Ptr & ", " & Off_R & " [inbounds]");
         end;
      else
         Emit_Inst (Ctx.E, FP & " = field_ptr " & Rec_Pir & ", " & Rec_Ptr & ", " & Img (FE.Field_Idx));
      end if;
      return FP;
   end Emit_Field_Addr;

   function Emit_Load_Field_Scalar
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Comp_Type : TREE;
      Comp_TI   : Type_Info;
      Prefix    : String := "fv") return String
   is
   begin
      if Is_Bitfield (FE) then
         return Emit_Bitfield_Load (Ctx, Rec_Ptr, FE, Comp_Type);
      end if;
      declare
         FP : constant String := Emit_Field_Addr (Ctx, Rec_Ptr, Rec_Pir, FE, Prefix & "p");
         FV : constant String := Next_Reg (Ctx.E, Prefix);
         Al : constant Natural := Explicit_Field_Align (FE, Comp_TI.Align);
      begin
         Emit_Inst (Ctx.E, FV & " = load." & To_String (Comp_TI.Pir_Type) & " " & FP & ", align " & Img (Al));
         return FV;
      end;
   end Emit_Load_Field_Scalar;

   procedure Emit_Store_Field_Scalar
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Val_Reg   : String;
      Comp_Type : TREE;
      Comp_TI   : Type_Info)
   is
   begin
      if Is_Bitfield (FE) then
         Emit_Bitfield_Store (Ctx, Rec_Ptr, FE, Val_Reg, To_String (Comp_TI.Pir_Type), Comp_Type);
      else
         declare
            FP : constant String := Emit_Field_Addr (Ctx, Rec_Ptr, Rec_Pir, FE, "fsp");
            Al : constant Natural := Explicit_Field_Align (FE, Comp_TI.Align);
         begin
            Emit_Inst (Ctx.E, "store." & To_String (Comp_TI.Pir_Type) & " " & Val_Reg & ", " & FP & ", align " & Img (Al));
         end;
      end if;
   end Emit_Store_Field_Scalar;

   function Emit_Load_Field_I64
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Comp_Type : TREE;
      Comp_TI   : Type_Info) return String
   is
      PT : constant String := To_String (Comp_TI.Pir_Type);
      FV : constant String := Emit_Load_Field_Scalar (Ctx, Rec_Ptr, Rec_Pir, FE, Comp_Type, Comp_TI, "dv");
   begin
      return Emit_Int_Convert (Ctx, FV, PT, "i64", Is_Unsigned => Is_Unsigned_Scalar (Comp_Type, PT));
   end Emit_Load_Field_I64;

   function Emit_Variant_Choice_Match
     (Ctx       : in out Context;
      DV64      : String;
      Disc_Type : TREE;
      Choice_S  : TREE;
      Is_Others : out Boolean) return String
   is
      CS  : SEQ_TYPE := LIST (Choice_S);
      Ch  : TREE;
      Hit : Unbounded_String;
   begin
      Is_Others := False;
      while not IS_EMPTY (CS) loop
         POP (CS, Ch);
         if KIND (Ch) = DN_CHOICE_OTHERS then
            Is_Others := True;
         elsif KIND (Ch) = DN_CHOICE_EXP then
            declare
               CV  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch), Disc_Type);
               C64 : constant String := Emit_Int_Convert (Ctx, To_String (CV.Reg), To_String (CV.Pir_Type), "i64",
                 Is_Unsigned => Is_Unsigned_Scalar (Disc_Type, To_String (CV.Pir_Type)));
               Eq  : constant String := Next_Reg (Ctx.E, "vceq");
            begin
               Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & DV64 & ", " & C64);
               if Length (Hit) = 0 then
                  Hit := To_Unbounded_String (Eq);
               else
                  declare
                     O : constant String := Next_Reg (Ctx.E, "vcor");
                  begin
                     Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & Eq);
                     Hit := To_Unbounded_String (O);
                  end;
               end if;
            end;
         elsif KIND (Ch) = DN_CHOICE_RANGE then
            declare
               RT2, LoR, HiR : Unbounded_String;
               Lo64, Hi64    : Unbounded_String;
               Ge  : constant String := Next_Reg (Ctx.E, "vcge");
               Le  : constant String := Next_Reg (Ctx.E, "vcle");
               InR : constant String := Next_Reg (Ctx.E, "vcin");
               RU  : Boolean;
            begin
               Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT2, LoR, HiR);
               RU   := Is_Unsigned_Scalar (Disc_Type, To_String (RT2));
               Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LoR), To_String (RT2), "i64", Is_Unsigned => RU));
               Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (HiR), To_String (RT2), "i64", Is_Unsigned => RU));
               Emit_Inst (Ctx.E, Ge & " = cmp_ge_s.i64 " & DV64 & ", " & To_String (Lo64));
               Emit_Inst (Ctx.E, Le & " = cmp_le_s.i64 " & DV64 & ", " & To_String (Hi64));
               Emit_Inst (Ctx.E, InR & " = and.i8 " & Ge & ", " & Le);
               if Length (Hit) = 0 then
                  Hit := To_Unbounded_String (InR);
               else
                  declare
                     O : constant String := Next_Reg (Ctx.E, "vcor");
                  begin
                     Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & InR);
                     Hit := To_Unbounded_String (O);
                  end;
               end if;
            end;
         end if;
      end loop;
      return To_String (Hit);
   end Emit_Variant_Choice_Match;

   procedure Emit_Record_Rep_Copy_Into
     (Ctx        : in out Context;
      Dst_Ptr    : String;
      Dst_T      : TREE;
      Src_Ptr    : String;
      Src_T      : TREE;
      Discs_Only : Boolean := False)
   is
      Src_BR : constant TREE := Subst_Base_Type (Ctx, Src_T);
      Dst_BR : constant TREE := Subst_Base_Type (Ctx, Dst_T);
      STI_R  : constant Type_Info := Get_Type_Info (Ctx, Src_BR);
      DTI_R  : constant Type_Info := Get_Type_Info (Ctx, Dst_BR);

      function Find_Comp (BR : TREE; Want : String) return TREE is
         function Search_DS (DS : TREE) return TREE is
            DL : SEQ_TYPE;
            DD : TREE;
         begin
            if Is_Void_Tree (DS) then return CONST_VOID; end if;
            DL := LIST (DS);
            while not IS_EMPTY (DL) loop
               POP (DL, DD);
               if KIND (DD) in DN_DSCRMT_DECL | DN_VARIABLE_DECL then
                  declare
                     NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                     ID : TREE;
                  begin
                     while not IS_EMPTY (NS) loop
                        POP (NS, ID);
                        if Sym_Name (ID) = Want then
                           return Canonical_Def (ID);
                        end if;
                     end loop;
                  end;
               end if;
            end loop;
            return CONST_VOID;
         end Search_DS;

         function Search_CL (CL : TREE) return TREE is
            R  : TREE;
            VP : TREE;
         begin
            if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return CONST_VOID; end if;
            R := Search_DS (D (AS_DECL_S, CL));
            if not Is_Void_Tree (R) then return R; end if;
            VP := D (AS_VARIANT_PART, CL);
            if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART
              and then not Is_Void_Tree (D (AS_VARIANT_S, VP))
            then
               declare
                  VL : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                  VN : TREE;
               begin
                  while not IS_EMPTY (VL) loop
                     POP (VL, VN);
                     if KIND (VN) = DN_VARIANT then
                        R := Search_CL (D (AS_COMP_LIST, VN));
                        if not Is_Void_Tree (R) then return R; end if;
                     end if;
                  end loop;
               end;
            end if;
            return CONST_VOID;
         end Search_CL;
         R : TREE;
      begin
         R := Search_DS (D (SM_DISCRIMINANT_S, BR));
         if not Is_Void_Tree (R) then return R; end if;
         return Search_CL (D (SM_COMP_LIST, BR));
      end Find_Comp;

      procedure Copy_Comp (Dst_CID : TREE) is
         Can_D   : constant TREE := Canonical_Def (Dst_CID);
         Src_CID : constant TREE := Find_Comp (Src_BR, Sym_Name (Can_D));
         DK      : constant Unsigned_32 := Tree_Key (Can_D);
         SK      : Unsigned_32;
      begin
         if Is_Void_Tree (Src_CID) or else not Ctx.Fields.Contains (DK) then return; end if;
         SK := Tree_Key (Src_CID);
         if not Ctx.Fields.Contains (SK) then return; end if;
         declare
            SFE  : constant Field_Entry := Ctx.Fields.Element (SK);
            DFE  : constant Field_Entry := Ctx.Fields.Element (DK);
            SCT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src_CID));
            SCTI : constant Type_Info := Get_Type_Info (Ctx, SCT);
            DCT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Can_D));
            DCTI : constant Type_Info := Get_Type_Info (Ctx, DCT);
         begin
            if DCTI.Kind = TK_Scalar and then SCTI.Kind = TK_Scalar then
               declare
                  VR : Unbounded_String :=
                    To_Unbounded_String (Emit_Load_Field_Scalar (Ctx, Src_Ptr, To_String (STI_R.Pir_Type), SFE, SCT, SCTI, "rcsv"));
               begin
                  if To_String (SCTI.Pir_Type) /= To_String (DCTI.Pir_Type) then
                     VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (VR), To_String (SCTI.Pir_Type), To_String (DCTI.Pir_Type),
                       Is_Unsigned => Is_Unsigned_Scalar (SCT, To_String (SCTI.Pir_Type))));
                  end if;
                  Emit_Store_Field_Scalar (Ctx, Dst_Ptr, To_String (DTI_R.Pir_Type), DFE, To_String (VR), DCT, DCTI);
               end;
            else
               declare
                  SP : constant String := Emit_Field_Addr (Ctx, Src_Ptr, To_String (STI_R.Pir_Type), SFE, "rcsp");
                  DP : constant String := Emit_Field_Addr (Ctx, Dst_Ptr, To_String (DTI_R.Pir_Type), DFE, "rcdp");
               begin
                  if SCTI.Kind = TK_Record and then DCTI.Kind = TK_Record
                    and then Subst_Base_Type (Ctx, SCT) /= Subst_Base_Type (Ctx, DCT)
                    and then (not Is_Void_Tree (D (SM_REPRESENTATION, Subst_Base_Type (Ctx, SCT)))
                              or else not Is_Void_Tree (D (SM_REPRESENTATION, Subst_Base_Type (Ctx, DCT)))
                              or else SCTI.Byte_Size /= DCTI.Byte_Size)
                  then
                     Emit_Record_Rep_Copy_Into (Ctx, DP, DCT, SP, SCT);
                  elsif DCTI.Kind = TK_Constrained_Array and then DCTI.Is_Dynamic then
                     -- Deep-copy dynamic constrained array descriptor
                     declare
                        SV : Expr_Val;
                     begin
                        Emit_Unpack_Arr_Desc (Ctx, SP, SCT, SV);
                        declare
                           ETI_D : constant Type_Info := Get_Type_Info (Ctx, DCTI.Elem_Type);
                           EAl_D : constant Natural := Elem_Storage_Align (ETI_D);
                           ESz_D : constant Natural := Elem_Storage_Size (ETI_D);
                           ND_D  : constant Natural := Natural'Max (1, Natural'Min (DCTI.NDims, Max_Array_Dims));
                           T_D, B_D : Unbounded_String;
                           Nz_D  : constant String := Next_Reg (Ctx.E, "rcnz");
                           Al_D  : constant String := Next_Reg (Ctx.E, "rcal");
                           NP_D  : Unbounded_String;
                           P0_D  : constant String := Next_Reg (Ctx.E, "rcp0");
                        begin
                           Emit_Inst (Ctx.E, "memcpy " & DP & ", " & SP & ", " & Img (Arr_Desc_Size) & ", align 1");
                           Emit_Array_Byte_Size_I64 (Ctx, SV.First_Regs, SV.Last_Regs, ND_D, ESz_D, T_D, B_D);
                           Emit_Inst (Ctx.E, Nz_D & " = cmp_gt_s.i64 " & To_String (B_D) & ", 0");
                           Emit_Inst (Ctx.E, Al_D & " = select.i64 " & Nz_D & ", " & To_String (B_D) & ", " & Img (EAl_D));
                           Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", Al_D, "ptr", NP_D);
                           Emit_Copy_Composite (Ctx, To_String (NP_D), SV, DCT);
                           Emit_Inst (Ctx.E, P0_D & " = field_ptr @__ada_arr_desc, " & DP & ", 0");
                           Emit_Inst (Ctx.E, "store.ptr " & To_String (NP_D) & ", " & P0_D & ", align 8");
                        end;
                     end;
                  else
                     declare
                        SV : Expr_Val;
                     begin
                        SV.Kind := EK_Addr; SV.Reg := To_Unbounded_String (SP);
                        SV.Pir_Type := SCTI.Pir_Type; SV.Ada_Type := SCT; SV.Align := 1;
                        Emit_Copy_Composite (Ctx, DP, SV, DCT);
                     end;
                  end if;
               end;
            end if;
         end;
      end Copy_Comp;

      procedure Copy_DS (DS : TREE) is
         DL : SEQ_TYPE;
         DD : TREE;
      begin
         if Is_Void_Tree (DS) then return; end if;
         DL := LIST (DS);
         while not IS_EMPTY (DL) loop
            POP (DL, DD);
            if KIND (DD) in DN_DSCRMT_DECL | DN_VARIABLE_DECL then
               declare
                  NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                  ID : TREE;
               begin
                  while not IS_EMPTY (NS) loop
                     POP (NS, ID);
                     Copy_Comp (ID);
                  end loop;
               end;
            end if;
         end loop;
      end Copy_DS;

      procedure Copy_CL (CL : TREE) is
         VP : TREE;
      begin
         if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return; end if;
         Copy_DS (D (AS_DECL_S, CL));
         VP := D (AS_VARIANT_PART, CL);
         if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART
           and then not Is_Void_Tree (D (AS_VARIANT_S, VP))
         then
            -- Dispatch ONLY the active variant matching the source discriminant!
            declare
               D_Name  : constant TREE := D (AS_NAME, VP);
               D_Def   : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D_Name)));
               Src_D   : constant TREE := Find_Comp (Src_BR, Sym_Name (D_Def));
               SD_K    : constant Unsigned_32 := (if not Is_Void_Tree (Src_D) then Tree_Key (Src_D) else 0);
            begin
               if SD_K /= 0 and then Ctx.Fields.Contains (SD_K) then
                  declare
                     SFE    : constant Field_Entry := Ctx.Fields.Element (SD_K);
                     DT_A   : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src_D));
                     DTI    : constant Type_Info := Get_Type_Info (Ctx, DT_A);
                     DV64   : constant String := Emit_Load_Field_I64 (Ctx, Src_Ptr, To_String (STI_R.Pir_Type), SFE, DT_A, DTI);
                     VL     : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                     VN     : TREE;
                     L_VEnd : constant String := Next_Label (Ctx.E, "rcve");
                  begin
                     while not IS_EMPTY (VL) loop
                        POP (VL, VN);
                        if KIND (VN) = DN_VARIANT then
                           declare
                              Has_Others : Boolean := False;
                              Hit        : constant String :=
                                Emit_Variant_Choice_Match (Ctx, DV64, DT_A, D (AS_CHOICE_S, VN), Has_Others);
                              L_Hit      : constant String := Next_Label (Ctx.E, "rcvh");
                              L_Nxt      : constant String := Next_Label (Ctx.E, "rcvn");
                           begin
                              if Has_Others then
                                 Copy_CL (D (AS_COMP_LIST, VN));
                                 Emit_Term (Ctx.E, "jmp " & L_VEnd);
                                 Emit_Label (Ctx.E, L_Nxt);
                              elsif Hit'Length > 0 then
                                 Emit_Term (Ctx.E, "br " & Hit & ", " & L_Hit & ", " & L_Nxt);
                                 Emit_Label (Ctx.E, L_Hit);
                                 Copy_CL (D (AS_COMP_LIST, VN));
                                 Emit_Term (Ctx.E, "jmp " & L_VEnd);
                                 Emit_Label (Ctx.E, L_Nxt);
                              end if;
                           end;
                        end if;
                     end loop;
                     Emit_Term (Ctx.E, "jmp " & L_VEnd);
                     Emit_Label (Ctx.E, L_VEnd);
                  end;
               end if;
            end;
         end if;
      end Copy_CL;
   begin
      if Is_Void_Tree (Src_BR) or else Is_Void_Tree (Dst_BR)
        or else KIND (Src_BR) /= DN_RECORD or else KIND (Dst_BR) /= DN_RECORD
      then
         return;
      end if;
      if DTI_R.Byte_Size > 0 then
         Emit_Inst (Ctx.E, "memset " & Dst_Ptr & ", 0, " & Img (DTI_R.Byte_Size) & ", align " & Img (Natural'Max (1, DTI_R.Align)));
      end if;
      Copy_DS (D (SM_DISCRIMINANT_S, Dst_BR));
      if not Discs_Only then
         Copy_CL (D (SM_COMP_LIST, Dst_BR));
      end if;
   end Emit_Record_Rep_Copy_Into;

   procedure Emit_Default_Init
     (Ctx            : in out Context;
      Dst_Ptr        : String;
      Dst_Type       : TREE;
      Activate_Tasks : Boolean := False)
   is
      RT  : constant TREE := Subst_Resolve_Type (Ctx, Dst_Type);
      BT  : constant TREE := Subst_Base_Type (Ctx, RT);
      DTI : constant Type_Info := Get_Type_Info (Ctx, RT);
   begin
      if Is_Void_Tree (RT) then
         return;
      end if;
      if Is_Task_Type (RT) then
         declare
            TH : constant String := Emit_Create_Task_Handle (Ctx, RT, Activate_Now => Activate_Tasks);
         begin
            Emit_Inst (Ctx.E, "store.ptr " & TH & ", " & Dst_Ptr & ", align 8");
         end;
         return;
      elsif Is_Access_Type (RT) then
         Emit_Inst (Ctx.E, "store.ptr 0, " & Dst_Ptr & ", align 8");
         return;
      elsif DTI.Kind = TK_Record and then not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
         -- Zero record storage so unused variant-union padding bytes are deterministic (e.g. c37215f)
         if DTI.Byte_Size > 0 then
            Emit_Inst (Ctx.E, "memset " & Dst_Ptr & ", 0, " & Img (DTI.Byte_Size) & ", align " & Img (Natural'Max (1, DTI.Align)));
         end if;
         -- 1. Discriminants (constrained values take precedence over default values)
         declare
            FIdx : Natural := 0;
            DS   : constant TREE := D (SM_DISCRIMINANT_S, BT);
            ND   : constant TREE := (if KIND (RT) = DN_CONSTRAINED_RECORD then D (SM_NORMALIZED_DSCRMT_S, RT) else CONST_VOID);
            NDL         : SEQ_TYPE := (if Is_Void_Tree (ND) then MAKE else LIST (ND));
            Bound_Discs : Temp_Var_Scope;
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
                           NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                           DID : TREE;
                           Def_E : constant TREE := D (AS_EXP, DD);
                        begin
                           while not IS_EMPTY (NS) loop
                              POP (NS, DID);
                              declare
                                 CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                 CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                 Init_E : TREE := D (SM_INIT_EXP, DID);
                                 FP  : constant String := Next_Reg (Ctx.E, "dfp");
                                 DK  : constant Unsigned_32 := Tree_Key (Canonical_Def (DID));
                                 DVInfo : Var_Info;
                              begin
                                 if Is_Void_Tree (Init_E) then
                                    Init_E := Def_E;
                                 end if;
                                 if not IS_EMPTY (NDL) then
                                    POP (NDL, Init_E);
                                 end if;
                                 -- P3-6.3 & P3-6.4: Honor record rep clauses & bitfields for discriminants in Emit_Default_Init!
                                 declare
                                    Has_DFE : constant Boolean := Ctx.Fields.Contains (DK);
                                    DFE     : constant Field_Entry := (if Has_DFE then Ctx.Fields.Element (DK) else (others => <>));
                                    Is_BF_D : constant Boolean := Has_DFE and then Is_Bitfield (DFE);
                                 begin
                                    if not Is_BF_D then
                                       if Has_DFE and then (DFE.In_Union or else DFE.Has_Explicit_Offset) then
                                          declare
                                             Off_R : constant String := Next_Reg (Ctx.E, "ddo");
                                          begin
                                             Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (DFE.Byte_Offset));
                                             Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                                          end;
                                       else
                                          Emit_Inst (Ctx.E,
                                            FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                            Dst_Ptr & ", " & Img (FIdx));
                                       end if;
                                    end if;
                                    if not Is_Void_Tree (Init_E) then
                                       declare
                                          VR : Unbounded_String;
                                       begin
                                          if KIND (RT) = DN_CONSTRAINED_RECORD
                                            and then Ctx.Saved_Type_Discs.Contains (Tree_Key (RT))
                                            and then (Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Is_Global
                                                      or else Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Owner_Subp = Ctx.Cur_Subp_Key)
                                            and then FIdx + 1 <= Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Count
                                          then
                                             declare
                                                SD : constant Saved_Disc_Info := Ctx.Saved_Type_Discs.Element (Tree_Key (RT));
                                                LV : constant String := Load_Saved_Slot (Ctx, To_String (SD.Ptrs (FIdx + 1)), To_String (SD.Pir_Types (FIdx + 1)), SD.Aligns (FIdx + 1), SD.Is_Global);
                                             begin
                                                VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, LV, To_String (SD.Pir_Types (FIdx + 1)), To_String (CTI.Pir_Type)));
                                             end;
                                          else
                                             declare
                                                DV : constant Expr_Val := Emit_Expr (Ctx, Init_E, CT);
                                                VP : constant String := To_String (DV.Pir_Type);
                                                DP : constant String := To_String (CTI.Pir_Type);
                                                VU : constant Boolean := Is_Unsigned_Scalar (DV.Ada_Type, VP);
                                             begin
                                                if Int_Bits_Of_Pir (VP) > Int_Bits_Of_Pir (DP) and then Is_Discrete_Type (CT) then
                                                   Emit_Range_Check (Ctx, To_String (DV.Reg), VP, CT);
                                                end if;
                                                VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (DV.Reg), VP, DP, Is_Unsigned => VU));
                                                Emit_Range_Check (Ctx, To_String (VR), DP, CT);
                                             end;
                                          end if;
                                          if Is_BF_D then
                                             Emit_Bitfield_Store (Ctx, Dst_Ptr, DFE, To_String (VR), To_String (CTI.Pir_Type), CT);
                                          else
                                             Emit_Inst (Ctx.E,
                                               "store." & To_String (CTI.Pir_Type) & " " & To_String (VR) &
                                               ", " & FP & ", align " & Img (if Has_DFE then Explicit_Field_Align (DFE, CTI.Align) else CTI.Align));
                                          end if;
                                       end;
                                    end if;
                                    -- Temporarily bind discriminant in Ctx.Vars so component default
                                    -- expressions referencing this discriminant read from FP / bitfield!
                                    DVInfo.Kind       := VK_Local_Alloca;
                                    DVInfo.Pir_Type   := CTI.Pir_Type;
                                    DVInfo.Ada_Type   := CT;
                                    DVInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                                    if Is_BF_D then
                                       DVInfo.Pir_Name    := To_Unbounded_String (Dst_Ptr);
                                       DVInfo.Is_Bitfield := True;
                                       DVInfo.BF_FE       := DFE;
                                       DVInfo.Align       := 1;
                                    else
                                       DVInfo.Pir_Name := To_Unbounded_String (FP);
                                       DVInfo.Align    := (if Has_DFE then Explicit_Field_Align (DFE, CTI.Align) else CTI.Align);
                                    end if;
                                    Bind_Temp_Var (Ctx, Bound_Discs, DK, DVInfo);
                                 end;
                              end;
                              FIdx := FIdx + 1;
                           end loop;
                        end;
                     end if;
                  end loop;
               end;
            end if;

            -- 2. Fixed record components (default expressions + recursive default init!)
            declare
               CL : constant TREE := D (SM_COMP_LIST, BT);
            begin
               if not Is_Void_Tree (CL) and then not Is_Void_Tree (D (AS_DECL_S, CL)) then
                  declare
                     DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                     Decl : TREE;
                  begin
                     while not IS_EMPTY (DL) loop
                        POP (DL, Decl);
                        if KIND (Decl) = DN_VARIABLE_DECL then
                           declare
                              NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Decl));
                              CID : TREE;
                              IE  : constant TREE := D (AS_EXP, Decl);
                           begin
                              while not IS_EMPTY (NS) loop
                                 POP (NS, CID);
                                 declare
                                    CID_K : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                    CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                                    RT_C : constant TREE := Resolve_Type_Spec (CT);
                                    CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                    FP  : constant String := Next_Reg (Ctx.E, "cfp");
                                    Has_SD_Save : Boolean := False;
                                    Prev_SD     : Saved_Disc_Info;
                                    Has_SR_Save : Boolean := False;
                                    Prev_SR     : Saved_Range_Info;
                                    SR_Key      : Unsigned_32 := 0;
                                 begin
                                    if not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Discs.Contains (CID_K) then
                                       if Ctx.Saved_Type_Discs.Contains (Tree_Key (RT_C)) then
                                          Has_SD_Save := True;
                                          Prev_SD := Ctx.Saved_Type_Discs.Element (Tree_Key (RT_C));
                                       end if;
                                       Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Ctx.Saved_Type_Discs.Element (CID_K));
                                    elsif not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Ranges.Contains (CID_K) then
                                       if KIND (RT_C) = DN_CONSTRAINED_ARRAY and then CTI.NDims >= 1 and then not Is_Void_Tree (CTI.Dims (1).Idx_Type) then
                                          SR_Key := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, CTI.Dims (1).Idx_Type)));
                                       else
                                          SR_Key := Tree_Key (RT_C);
                                       end if;
                                       if Ctx.Saved_Type_Ranges.Contains (SR_Key) then
                                          Has_SR_Save := True;
                                          Prev_SR := Ctx.Saved_Type_Ranges.Element (SR_Key);
                                       end if;
                                       Ctx.Saved_Type_Ranges.Include (SR_Key, Ctx.Saved_Type_Ranges.Element (CID_K));
                                    end if;
                                    if CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                                       -- Allocate & initialize embedded @__ada_arr_desc with discriminants bound! (RC-8)
                                       Emit_Inst (Ctx.E,
                                         FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                         Dst_Ptr & ", " & Img (FIdx));
                                       declare
                                          ETI   : constant Type_Info := Get_Type_Info (Ctx, CTI.Elem_Type);
                                          ESz   : constant Natural := Elem_Storage_Size (ETI);
                                          ND_A  : constant Natural := Natural'Max (1, Natural'Min (CTI.NDims, Max_Array_Dims));
                                          BaseA : constant TREE := Resolve_Type_Spec (Base_Type_Spec (CT));
                                          BTI_A : constant Type_Info := Get_Type_Info (Ctx, BaseA);
                                          Firsts, Lasts : Str_Array_4;
                                          Tot64 : Unbounded_String;
                                          Byt   : constant String := Next_Reg (Ctx.E, "daby");
                                          Nz    : constant String := Next_Reg (Ctx.E, "danz");
                                          AlB   : constant String := Next_Reg (Ctx.E, "daal");
                                          DataP : Unbounded_String;
                                       begin
                                          for D_I in 1 .. ND_A loop
                                             Emit_Index_Bounds (Ctx, CTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                                             -- Check non-null bounds belong to base array index subtype (c32114a, c32117a)
                                             if Ctx.Enable_Checks and then D_I <= BTI_A.NDims and then not Is_Void_Tree (BTI_A.Dims (D_I).Idx_Type) then
                                                Emit_Non_Null_Index_Check (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)), BTI_A.Dims (D_I).Idx_Type);
                                             end if;
                                          end loop;
                                          declare
                                             Byt_U : Unbounded_String;
                                          begin
                                             Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND_A, ESz, Tot64, Byt_U);
                                             Emit_Inst (Ctx.E, Byt & " = add.i64 " & To_String (Byt_U) & ", 0");
                                          end;
                                          Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                                          Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Elem_Storage_Align (ETI)));
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
                                          Emit_Store_Arr_Desc (Ctx, FP, To_String (DataP), Firsts, Lasts, ND_A);
                                          if not Is_Void_Tree (IE) then
                                             if KIND (IE) = DN_AGGREGATE then
                                                Emit_Init_Composite (Ctx, To_String (DataP), CT, IE, Firsts, Lasts);
                                             else
                                                declare
                                                   SV  : constant Expr_Val := Emit_Expr (Ctx, IE, CT);
                                                   Tgt : Expr_Val;
                                                begin
                                                   Tgt.Kind := EK_Slice; Tgt.Reg := DataP; Tgt.Ada_Type := CT;
                                                   Tgt.NDims := ND_A; Tgt.First_Regs := Firsts; Tgt.Last_Regs := Lasts;
                                                   declare
                                                      IE_U : TREE := IE;
                                                   begin
                                                      while not Is_Void_Tree (IE_U) and then KIND (IE_U) = DN_PARENTHESIZED loop
                                                         IE_U := D (AS_EXP, IE_U);
                                                      end loop;
                                                      if not Is_Void_Tree (IE_U) and then KIND (IE_U) in DN_AGGREGATE | DN_STRING_LITERAL then
                                                         Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                                                      elsif Ctx.Enable_Checks then
                                                         declare
                                                            SV_W : constant Array_View := Resolve_Array_View (Ctx, SV);
                                                         begin
                                                            Emit_Exact_Bounds_Check (Ctx, SV_W.Firsts, SV_W.Lasts, Firsts, Lasts, ND_A);
                                                         end;
                                                      end if;
                                                   end;
                                                   Emit_Inst (Ctx.E, "memmove " & To_String (DataP) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                                                end;
                                             end if;
                                          elsif Is_Access_Type (CTI.Elem_Type) or else Is_Record_Type (CTI.Elem_Type)
                                            or else Is_Task_Type (CTI.Elem_Type) or else ETI.Kind = TK_Constrained_Array
                                          then
                                             declare
                                                L_H : constant String := Next_Label (Ctx.E, "ddi_h");
                                                L_B : constant String := Next_Label (Ctx.E, "ddi_b");
                                                L_E : constant String := Next_Label (Ctx.E, "ddi_e");
                                                IR  : constant String := Next_Reg (Ctx.E, "ddi");
                                                Cnd : constant String := Next_Reg (Ctx.E, "ddc");
                                                EP  : constant String := Next_Reg (Ctx.E, "ddep");
                                                Nx  : constant String := Next_Reg (Ctx.E, "ddnx");
                                             begin
                                                Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                                                Emit_Label (Ctx.E, L_H, IR & ": i64");
                                                Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                                                Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                                                Emit_Label (Ctx.E, L_B);
                                                declare
                                                   EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (DataP), IR);
                                                begin
                                                   Emit_Default_Init (Ctx, EP2, CTI.Elem_Type, Activate_Tasks => Activate_Tasks);
                                                end;
                                                Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                                                Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                                                Emit_Label (Ctx.E, L_E);
                                             end;
                                          end if;
                                       end;
                                    elsif not Is_Void_Tree (IE) then
                                       declare
                                          CK_D : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                          Has_FE_D : constant Boolean := Ctx.Fields.Contains (CK_D);
                                          FE_D : constant Field_Entry := (if Has_FE_D then Ctx.Fields.Element (CK_D) else (others => <>));
                                       begin
                                          if Has_FE_D and then (FE_D.In_Union or else FE_D.Has_Explicit_Offset) then
                                             declare
                                                Off_R : constant String := Next_Reg (Ctx.E, "dfo");
                                             begin
                                                Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE_D.Byte_Offset));
                                                Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                                             end;
                                          else
                                             Emit_Inst (Ctx.E,
                                               FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                               Dst_Ptr & ", " & Img (FIdx));
                                          end if;
                                          if CTI.Kind = TK_Scalar then
                                             declare
                                                V  : constant Expr_Val := Emit_Expr (Ctx, IE, CT);
                                                VP : constant String := To_String (V.Pir_Type);
                                                DP : constant String := To_String (CTI.Pir_Type);
                                                VU : constant Boolean := Is_Unsigned_Scalar (V.Ada_Type, VP);
                                                VR : Unbounded_String;
                                             begin
                                                if Int_Bits_Of_Pir (VP) > Int_Bits_Of_Pir (DP) and then Is_Discrete_Type (CT) then
                                                   Emit_Range_Check (Ctx, To_String (V.Reg), VP, CT);
                                                end if;
                                                VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (V.Reg), VP, DP, Is_Unsigned => VU));
                                                Emit_Range_Check (Ctx, To_String (VR), DP, CT);
                                                Emit_Discriminant_Check (Ctx, To_String (VR), CT, Is_Access => True);
                                                if Has_FE_D and then Is_Bitfield (FE_D) then
                                                   Emit_Bitfield_Store (Ctx, Dst_Ptr, FE_D, To_String (VR), DP, CT);
                                                else
                                                   Emit_Inst (Ctx.E,
                                                     "store." & DP & " " & To_String (VR) &
                                                     ", " & FP & ", align " & Img (if Has_FE_D then Explicit_Field_Align (FE_D, CTI.Align) else CTI.Align));
                                                end if;
                                             end;
                                          else
                                             declare
                                                IE_U : TREE := IE;
                                                Need_Exact : Boolean := False;
                                             begin
                                                while not Is_Void_Tree (IE_U) and then KIND (IE_U) = DN_PARENTHESIZED loop
                                                   IE_U := D (AS_EXP, IE_U);
                                                end loop;
                                                if CTI.Kind = TK_Constrained_Array
                                                  and then (Is_Void_Tree (IE_U) or else KIND (IE_U) not in DN_AGGREGATE | DN_STRING_LITERAL)
                                                then
                                                   Need_Exact := True;
                                                end if;
                                                Emit_Init_Composite (Ctx, FP, CT, IE, Exact_Bounds => Need_Exact);
                                                if CTI.Kind = TK_Record then
                                                   Emit_Discriminant_Check (Ctx, FP, CT, Is_Access => False);
                                                end if;
                                             end;
                                          end if;
                                       end;
                                    elsif Is_Access_Type (Subst_Resolve_Type (Ctx, CT)) or else CTI.Kind = TK_Record
                                      or else Is_Task_Type (Subst_Resolve_Type (Ctx, CT))
                                      or else CTI.Kind = TK_Constrained_Array
                                    then
                                       declare
                                          CK_D : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                       begin
                                          if Ctx.Fields.Contains (CK_D)
                                            and then (Ctx.Fields.Element (CK_D).In_Union or else Ctx.Fields.Element (CK_D).Has_Explicit_Offset)
                                          then
                                             declare
                                                Off_R : constant String := Next_Reg (Ctx.E, "dfo");
                                             begin
                                                Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (Ctx.Fields.Element (CK_D).Byte_Offset));
                                                Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                                             end;
                                          else
                                             Emit_Inst (Ctx.E,
                                               FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                               Dst_Ptr & ", " & Img (FIdx));
                                          end if;
                                       end;
                                       Emit_Default_Init (Ctx, FP, CT, Activate_Tasks => Activate_Tasks);
                                    end if;
                                    if not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Discs.Contains (CID_K) then
                                       if Has_SD_Save then
                                          Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Prev_SD);
                                       else
                                          Ctx.Saved_Type_Discs.Exclude (Tree_Key (RT_C));
                                       end if;
                                    end if;
                                    if SR_Key /= 0 then
                                       if Has_SR_Save then
                                          Ctx.Saved_Type_Ranges.Include (SR_Key, Prev_SR);
                                       else
                                          Ctx.Saved_Type_Ranges.Exclude (SR_Key);
                                       end if;
                                    end if;
                                 end;
                                 FIdx := FIdx + 1;
                              end loop;
                           end;
                        end if;
                     end loop;
                  end;
               end if;
               -- 2b. Variant part default initialization (recursive for nested variants: c48005a, c48012a, c52008b, c64108a, c95089a)
               declare
                  procedure Emit_Variant_Part_Default_Init (VP : TREE) is
                  begin
                     if Is_Void_Tree (VP) or else KIND (VP) /= DN_VARIANT_PART then
                        return;
                     end if;
                     declare
                        DDef  : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, VP))));
                        VS    : constant TREE := D (AS_VARIANT_S, VP);
                        VL    : SEQ_TYPE := (if not Is_Void_Tree (VS) then LIST (VS) else MAKE);
                        VN    : TREE;
                        L_End : constant String := Next_Label (Ctx.E, "vdi_e");
                     begin
                        if Ctx.Vars.Contains (Tree_Key (DDef)) then
                           declare
                              DV  : constant Var_Info := Ctx.Vars.Element (Tree_Key (DDef));
                              D64 : Unbounded_String;
                           begin
                              if DV.Is_Bitfield then
                                 declare
                                    BV : constant String := Emit_Bitfield_Load (Ctx, To_String (DV.Pir_Name), DV.BF_FE, DV.Ada_Type);
                                 begin
                                    D64 := To_Unbounded_String (Emit_Int_Convert (Ctx, BV, To_String (DV.Pir_Type), "i64",
                                      Is_Unsigned => Is_Unsigned_Scalar (DV.Ada_Type, To_String (DV.Pir_Type))));
                                 end;
                              else
                                 declare
                                    DVal : constant String := Next_Reg (Ctx.E, "vdv");
                                 begin
                                    Emit_Inst (Ctx.E, DVal & " = load." & To_String (DV.Pir_Type) & " " & To_String (DV.Pir_Name) & ", align " & Img (DV.Align));
                                    D64 := To_Unbounded_String (Emit_Int_Convert (Ctx, DVal, To_String (DV.Pir_Type), "i64",
                                      Is_Unsigned => Is_Unsigned_Scalar (DV.Ada_Type, To_String (DV.Pir_Type))));
                                 end;
                              end if;
                              while not IS_EMPTY (VL) loop
                                 POP (VL, VN);
                                 if KIND (VN) = DN_VARIANT then
                                    declare
                                       Is_Ot : Boolean := False;
                                       Cond  : constant String :=
                                         Emit_Variant_Choice_Match (Ctx, To_String (D64), DV.Ada_Type, D (AS_CHOICE_S, VN), Is_Ot);
                                       L_Hit : constant String := Next_Label (Ctx.E, "vdi_h");
                                       L_Nxt : constant String := Next_Label (Ctx.E, "vdi_n");
                                    begin
                                       if Is_Ot then
                                          Emit_Term (Ctx.E, "jmp " & L_Hit);
                                       elsif Cond'Length > 0 then
                                          Emit_Term (Ctx.E, "br " & Cond & ", " & L_Hit & ", " & L_Nxt);
                                       else
                                          Emit_Term (Ctx.E, "jmp " & L_Nxt);
                                       end if;
                                       Emit_Label (Ctx.E, L_Hit);
                                       -- Initialize variant's components using Ctx.Fields byte offsets
                                       declare
                                          VCL : constant TREE := D (AS_COMP_LIST, VN);
                                       begin
                                          if not Is_Void_Tree (VCL) then
                                             if not Is_Void_Tree (D (AS_DECL_S, VCL)) then
                                                declare
                                                   VDL : SEQ_TYPE := LIST (D (AS_DECL_S, VCL));
                                                   VDecl : TREE;
                                                begin
                                                   while not IS_EMPTY (VDL) loop
                                                      POP (VDL, VDecl);
                                                      if KIND (VDecl) = DN_VARIABLE_DECL then
                                                         declare
                                                            VNS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, VDecl));
                                                            VCID : TREE;
                                                            VIE  : constant TREE := D (AS_EXP, VDecl);
                                                         begin
                                                            while not IS_EMPTY (VNS) loop
                                                               POP (VNS, VCID);
                                                               declare
                                                                  VCT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, VCID));
                                                                  VRT_C : constant TREE := Resolve_Type_Spec (VCT);
                                                                  VCTI : constant Type_Info := Get_Type_Info (Ctx, VCT);
                                                                  VCK  : constant Unsigned_32 := Tree_Key (Canonical_Def (VCID));
                                                                  V_Has_SD : Boolean := False;
                                                                  V_Prev_SD : Saved_Disc_Info;
                                                                  V_Has_SR : Boolean := False;
                                                                  V_Prev_SR : Saved_Range_Info;
                                                                  V_SR_Key  : Unsigned_32 := 0;
                                                               begin
                                                                  if not Is_Void_Tree (VRT_C) and then Ctx.Saved_Type_Discs.Contains (VCK) then
                                                                     if Ctx.Saved_Type_Discs.Contains (Tree_Key (VRT_C)) then
                                                                        V_Has_SD  := True;
                                                                        V_Prev_SD := Ctx.Saved_Type_Discs.Element (Tree_Key (VRT_C));
                                                                     end if;
                                                                     Ctx.Saved_Type_Discs.Include (Tree_Key (VRT_C), Ctx.Saved_Type_Discs.Element (VCK));
                                                                  elsif not Is_Void_Tree (VRT_C) and then Ctx.Saved_Type_Ranges.Contains (VCK) then
                                                                     if KIND (VRT_C) = DN_CONSTRAINED_ARRAY and then VCTI.NDims >= 1 and then not Is_Void_Tree (VCTI.Dims (1).Idx_Type) then
                                                                        V_SR_Key := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, VCTI.Dims (1).Idx_Type)));
                                                                     else
                                                                        V_SR_Key := Tree_Key (VRT_C);
                                                                     end if;
                                                                     if Ctx.Saved_Type_Ranges.Contains (V_SR_Key) then
                                                                        V_Has_SR  := True;
                                                                        V_Prev_SR := Ctx.Saved_Type_Ranges.Element (V_SR_Key);
                                                                     end if;
                                                                     Ctx.Saved_Type_Ranges.Include (V_SR_Key, Ctx.Saved_Type_Ranges.Element (VCK));
                                                                  end if;
                                                                  if Ctx.Fields.Contains (VCK) then
                                                                     declare
                                                                        FE  : constant Field_Entry := Ctx.Fields.Element (VCK);
                                                                        FP  : constant String := Next_Reg (Ctx.E, "vfp");
                                                                     begin
                                                                        if FE.In_Union or else FE.Has_Explicit_Offset then
                                                                           declare
                                                                              Off_R : constant String := Next_Reg (Ctx.E, "vfo");
                                                                           begin
                                                                              Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                                                                              Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                                                                           end;
                                                                        else
                                                                           Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " & Dst_Ptr & ", " & Img (FE.Field_Idx));
                                                                        end if;
                                                                        if VCTI.Kind = TK_Constrained_Array and then VCTI.Is_Dynamic then
                                                                           declare
                                                                              ETI   : constant Type_Info := Get_Type_Info (Ctx, VCTI.Elem_Type);
                                                                              Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
                                                                              EAl   : constant Natural := (if Is_DE then 8 else Natural'Max (1, ETI.Align));
                                                                              ESz   : constant Natural := Elem_Storage_Size (ETI);
                                                                              ND_A  : constant Natural := Natural'Max (1, Natural'Min (VCTI.NDims, Max_Array_Dims));
                                                                              Firsts, Lasts : Str_Array_4;
                                                                              Tot64 : Unbounded_String;
                                                                              Byt   : Unbounded_String;
                                                                              Nz    : constant String := Next_Reg (Ctx.E, "vanz");
                                                                              AlB   : constant String := Next_Reg (Ctx.E, "vaal");
                                                                              DataP : Unbounded_String;
                                                                           begin
                                                                              for D_I in 1 .. ND_A loop
                                                                                 Emit_Index_Bounds (Ctx, VCTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                                                                                 if Ctx.Enable_Checks then
                                                                                    declare
                                                                                       BA  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (VCT));
                                                                                       BTI : constant Type_Info := (if not Is_Void_Tree (BA) and then KIND (BA) = DN_ARRAY then Get_Type_Info (Ctx, BA) else VCTI);
                                                                                    begin
                                                                                       if D_I <= BTI.NDims and then not Is_Void_Tree (BTI.Dims (D_I).Idx_Type) then
                                                                                          Emit_Non_Null_Index_Check (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)), BTI.Dims (D_I).Idx_Type);
                                                                                       end if;
                                                                                    end;
                                                                                 end if;
                                                                              end loop;
                                                                              Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND_A, ESz, Tot64, Byt);
                                                                              Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                                                                              Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                                                                              Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
                                                                              Emit_Store_Arr_Desc (Ctx, FP, To_String (DataP), Firsts, Lasts, ND_A);
                                                                              if not Is_Void_Tree (VIE) then
                                                                                 Emit_Init_Composite (Ctx, To_String (DataP), VCT, VIE, Firsts, Lasts);
                                                                              elsif Is_Access_Type (VCTI.Elem_Type) or else Is_Record_Type (VCTI.Elem_Type)
                                                                                or else Is_Task_Type (VCTI.Elem_Type) or else ETI.Kind = TK_Constrained_Array
                                                                              then
                                                                                 declare
                                                                                    L_H : constant String := Next_Label (Ctx.E, "vdi_h");
                                                                                    L_B : constant String := Next_Label (Ctx.E, "vdi_b");
                                                                                    L_E : constant String := Next_Label (Ctx.E, "vdi_e");
                                                                                    IR  : constant String := Next_Reg (Ctx.E, "vdi");
                                                                                    Cnd : constant String := Next_Reg (Ctx.E, "vdc");
                                                                                    Nx  : constant String := Next_Reg (Ctx.E, "vdnx");
                                                                                 begin
                                                                                    Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                                                                                    Emit_Label (Ctx.E, L_H, IR & ": i64");
                                                                                    Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                                                                                    Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                                                                                    Emit_Label (Ctx.E, L_B);
                                                                                    declare
                                                                                       EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (DataP), IR);
                                                                                    begin
                                                                                       Emit_Default_Init (Ctx, EP2, VCTI.Elem_Type, Activate_Tasks => Activate_Tasks);
                                                                                    end;
                                                                                    Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                                                                                    Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                                                                                    Emit_Label (Ctx.E, L_E);
                                                                                 end;
                                                                              end if;
                                                                           end;
                                                                        elsif not Is_Void_Tree (VIE) then
                                                                           if VCTI.Kind = TK_Scalar then
                                                                              declare
                                                                                 VV : constant Expr_Val := Emit_Expr (Ctx, VIE, VCT);
                                                                                 VP : constant String := To_String (VV.Pir_Type);
                                                                                 DP : constant String := To_String (VCTI.Pir_Type);
                                                                                 VU : constant Boolean := Is_Unsigned_Scalar (VV.Ada_Type, VP);
                                                                                 VR : Unbounded_String;
                                                                              begin
                                                                                 if Int_Bits_Of_Pir (VP) > Int_Bits_Of_Pir (DP) and then Is_Discrete_Type (VCT) then
                                                                                    Emit_Range_Check (Ctx, To_String (VV.Reg), VP, VCT);
                                                                                 end if;
                                                                                 VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (VV.Reg), VP, DP, Is_Unsigned => VU));
                                                                                 Emit_Range_Check (Ctx, To_String (VR), DP, VCT);
                                                                                 Emit_Discriminant_Check (Ctx, To_String (VR), VCT, Is_Access => True);
                                                                                 if Is_Bitfield (FE) then
                                                                                    Emit_Bitfield_Store (Ctx, Dst_Ptr, FE, To_String (VR), DP, VCT);
                                                                                 else
                                                                                    Emit_Inst (Ctx.E, "store." & DP & " " & To_String (VR) & ", " & FP & ", align " & Img (Explicit_Field_Align (FE, VCTI.Align)));
                                                                                 end if;
                                                                              end;
                                                                           else
                                                                              Emit_Init_Composite (Ctx, FP, VCT, VIE);
                                                                              if VCTI.Kind = TK_Record then
                                                                                 Emit_Discriminant_Check (Ctx, FP, VCT, Is_Access => False);
                                                                              end if;
                                                                           end if;
                                                                        elsif Is_Access_Type (Subst_Resolve_Type (Ctx, VCT)) or else VCTI.Kind = TK_Record
                                                                          or else Is_Task_Type (Subst_Resolve_Type (Ctx, VCT))
                                                                          or else VCTI.Kind = TK_Constrained_Array
                                                                        then
                                                                           Emit_Default_Init (Ctx, FP, VCT, Activate_Tasks => Activate_Tasks);
                                                                        end if;
                                                                     end;
                                                                  end if;
                                                                  if not Is_Void_Tree (VRT_C) and then Ctx.Saved_Type_Discs.Contains (VCK) then
                                                                     if V_Has_SD then
                                                                        Ctx.Saved_Type_Discs.Include (Tree_Key (VRT_C), V_Prev_SD);
                                                                     else
                                                                        Ctx.Saved_Type_Discs.Exclude (Tree_Key (VRT_C));
                                                                     end if;
                                                                  end if;
                                                                  if V_SR_Key /= 0 then
                                                                     if V_Has_SR then
                                                                        Ctx.Saved_Type_Ranges.Include (V_SR_Key, V_Prev_SR);
                                                                     else
                                                                        Ctx.Saved_Type_Ranges.Exclude (V_SR_Key);
                                                                     end if;
                                                                  end if;
                                                               end;
                                                            end loop;
                                                         end;
                                                      end if;
                                                   end loop;
                                                end;
                                             end if;
                                             if not Is_Void_Tree (D (AS_VARIANT_PART, VCL)) then
                                                Emit_Variant_Part_Default_Init (D (AS_VARIANT_PART, VCL));
                                             end if;
                                          end if;
                                       end;
                                       Emit_Term (Ctx.E, "jmp " & L_End);
                                       Emit_Label (Ctx.E, L_Nxt);
                                    end;
                                 end if;
                              end loop;
                              Emit_Term (Ctx.E, "jmp " & L_End);
                              Emit_Label (Ctx.E, L_End);
                           end;
                        end if;
                     end;
                  end Emit_Variant_Part_Default_Init;
               begin
                  if not Is_Void_Tree (CL) and then not Is_Void_Tree (D (AS_VARIANT_PART, CL)) then
                     Emit_Variant_Part_Default_Init (D (AS_VARIANT_PART, CL));
                  end if;
               end;
            end;

            -- Clean up temporary discriminant bindings (restoring any outer bindings!)
            Restore_Temp_Vars (Ctx, Bound_Discs);
         end;

      elsif DTI.Kind = TK_Constrained_Array and then DTI.Is_Dynamic then
         -- Embedded @__ada_arr_desc for a dynamically-constrained array element (c32001b, c42007g, c43205k, c43214f, cd2a81a)
         declare
            ETI   : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
            Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
            EAl   : constant Natural := (if Is_DE then 8 else Natural'Max (1, ETI.Align));
            ESz   : constant Natural := Elem_Storage_Size (ETI);
            ND_A  : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
            Firsts, Lasts : Str_Array_4;
            Tot64 : Unbounded_String;
            Byt   : constant String := Next_Reg (Ctx.E, "eaby");
            Nz    : constant String := Next_Reg (Ctx.E, "eanz");
            AlB   : constant String := Next_Reg (Ctx.E, "eaal");
            DataP : Unbounded_String;
         begin
            for D_I in 1 .. ND_A loop
               Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
            end loop;
            declare
               Byt_U : Unbounded_String;
            begin
               Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND_A, ESz, Tot64, Byt_U);
               Emit_Inst (Ctx.E, Byt & " = add.i64 " & To_String (Byt_U) & ", 0");
            end;
            Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
            Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (EAl));
            Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
            Emit_Store_Arr_Desc (Ctx, Dst_Ptr, To_String (DataP), Firsts, Lasts, ND_A);
            if Is_Access_Type (Subst_Resolve_Type (Ctx, DTI.Elem_Type)) or else ETI.Kind = TK_Record
              or else Is_Task_Type (Subst_Resolve_Type (Ctx, DTI.Elem_Type)) or else ETI.Kind = TK_Constrained_Array
            then
               declare
                  L_H : constant String := Next_Label (Ctx.E, "eadi_h");
                  L_B : constant String := Next_Label (Ctx.E, "eadi_b");
                  L_E : constant String := Next_Label (Ctx.E, "eadi_e");
                  IR  : constant String := Next_Reg (Ctx.E, "eadi");
                  Cnd : constant String := Next_Reg (Ctx.E, "eadc");
                  Nx  : constant String := Next_Reg (Ctx.E, "eadn");
               begin
                  Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                  Emit_Label (Ctx.E, L_H, IR & ": i64");
                  Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                  Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                  Emit_Label (Ctx.E, L_B);
                  declare
                     EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (DataP), IR);
                  begin
                     Emit_Default_Init (Ctx, EP2, DTI.Elem_Type, Activate_Tasks => Activate_Tasks);
                  end;
                  Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                  Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                  Emit_Label (Ctx.E, L_E);
               end;
            end if;
         end;

      elsif DTI.Kind = TK_Constrained_Array and then not DTI.Is_Dynamic then
         -- Only emit default init loop for arrays whose elements need default init (access / record / nested array)
         declare
            ETI : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
         begin
            if Is_Access_Type (DTI.Elem_Type) or else Is_Record_Type (DTI.Elem_Type)
              or else Is_Task_Type (DTI.Elem_Type)
              or else ETI.Kind = TK_Constrained_Array
            then
               declare
                  N_Tot : Integer_64 := 1;
               begin
                  for D_Idx in 1 .. DTI.NDims loop
                     if DTI.Dims (D_Idx).Last >= DTI.Dims (D_Idx).First then
                        N_Tot := N_Tot * (DTI.Dims (D_Idx).Last - DTI.Dims (D_Idx).First + 1);
                     else
                        N_Tot := 0;
                     end if;
                  end loop;
                  if N_Tot > 0 and then N_Tot <= 16_777_216 then
                     declare
                        L_Hdr  : constant String := Next_Label (Ctx.E, "di_h");
                        L_Bdy  : constant String := Next_Label (Ctx.E, "di_b");
                        L_Done : constant String := Next_Label (Ctx.E, "di_e");
                        I_Reg  : constant String := Next_Reg (Ctx.E, "di");
                        Cond   : constant String := Next_Reg (Ctx.E, "dc");
                        EP     : constant String := Next_Reg (Ctx.E, "dep");
                        Nx     : constant String := Next_Reg (Ctx.E, "din");
                     begin
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(0)");
                        Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                        Emit_Inst (Ctx.E, Cond & " = cmp_lt_s.i64 " & I_Reg & ", " & Img (Long_Long_Integer (N_Tot)));
                        Emit_Term (Ctx.E, "br " & Cond & ", " & L_Bdy & ", " & L_Done);
                        Emit_Label (Ctx.E, L_Bdy);
                        declare
                           EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, Dst_Ptr, I_Reg);
                        begin
                           Emit_Default_Init (Ctx, EP2, DTI.Elem_Type, Activate_Tasks => Activate_Tasks);
                        end;
                        Emit_Inst (Ctx.E, Nx & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nx & ")");
                        Emit_Label (Ctx.E, L_Done);
                     end;
                  end if;
               end;
            elsif DTI.Byte_Size > 0 and then DTI.Byte_Size <= 65_536 then
               -- Zero uninitialized local scalar/enum arrays (e.g. String in cc3126a) so
               -- reading an uninitialized Character element cannot hit stack garbage > 127.
               Emit_Inst (Ctx.E, "memset " & Dst_Ptr & ", 0, " & Img (DTI.Byte_Size) & ", align " & Img (Natural'Max (1, DTI.Align)));
            end if;
         end;
      end if;
   end Emit_Default_Init;

end Cg_Expr.Composite;
