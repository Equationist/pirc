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

   function Hash_U64 (K : Unsigned_64) return Ada.Containers.Hash_Type is
   begin
      return Ada.Containers.Hash_Type ((K xor Shift_Right (K, 32)) and 16#FFFF_FFFF#);
   end Hash_U64;

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

   function Subst_Resolve_Type (Ctx : Context; T : TREE) return TREE is
      Cur : TREE := T;
      Nxt : TREE;
   begin
      if Is_Void_Tree (Cur) then
         return CONST_VOID;
      end if;
      for Step in 1 .. 16 loop
         Nxt := Resolve_Type_Spec (Subst_Tree (Ctx, Cur));
         if Is_Void_Tree (Nxt) then
            return CONST_VOID;
         end if;
         if Nxt = Cur then
            if KIND (Cur) in DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED | DN_RECORD | DN_PRIVATE | DN_L_PRIVATE | DN_ARRAY | DN_ACCESS then
               declare
                  Der : constant TREE := D (SM_DERIVED, Cur);
               begin
                  if not Is_Void_Tree (Der) then
                     declare
                        SDer : constant TREE := Subst_Tree (Ctx, Der);
                     begin
                        if SDer /= Der and then not Is_Void_Tree (SDer) then
                           if KIND (Cur) = DN_ENUMERATION
                             and then (Is_Void_Tree (D (SM_LITERAL_S, Cur)) or else IS_EMPTY (LIST (D (SM_LITERAL_S, Cur))))
                           then
                              Cur := SDer;
                              goto Continue_Loop;
                           elsif KIND (Cur) = DN_ARRAY then
                              Cur := SDer;
                              goto Continue_Loop;
                           elsif KIND (Cur) in DN_INTEGER | DN_FLOAT | DN_FIXED
                             and then (Is_Void_Tree (D (SM_RANGE, Cur))
                                       or else D (SM_RANGE, Cur) = D (SM_RANGE, Der)
                                       or else (KIND (D (SM_RANGE, Cur)) = DN_RANGE
                                                and then not Has_Static_Val (Safe_SM_Value (D (AS_EXP1, D (SM_RANGE, Cur))))))
                           then
                              Cur := SDer;
                              goto Continue_Loop;
                           elsif KIND (Cur) in DN_PRIVATE | DN_L_PRIVATE | DN_RECORD | DN_ACCESS
                             and then KIND (Resolve_Type_Spec (SDer)) /= KIND (Cur)
                           then
                              Cur := SDer;
                              goto Continue_Loop;
                           end if;
                        end if;
                     end;
                  end if;
               end;
            end if;
            exit;
         end if;
         Cur := Nxt;
         <<Continue_Loop>>
      end loop;
      return Cur;
   end Subst_Resolve_Type;

   function Base_Of_Type (Ctx : Context; T : TREE) return TREE is
      Cur : TREE := Subst_Resolve_Type (Ctx, T);
      B   : TREE;
   begin
      if Is_Void_Tree (Cur) then
         return CONST_VOID;
      end if;
      for Step in 1 .. 16 loop
         B := Base_Type_Spec (Cur);
         if not Is_Void_Tree (B) then
            B := Subst_Resolve_Type (Ctx, B);
         end if;
         exit when Is_Void_Tree (B) or else B = Cur;
         Cur := B;
      end loop;
      return Cur;
   end Base_Of_Type;

   function Root_Base_Type (Ctx : Context; T : TREE) return TREE is
      Cur : TREE := Subst_Resolve_Type (Ctx, T);
      B   : TREE;
   begin
      if Is_Void_Tree (Cur) then
         return CONST_VOID;
      end if;
      for Step in 1 .. 16 loop
         B := Base_Type_Spec (Cur);
         if Is_Void_Tree (B) then
            exit;
         end if;
         B := Subst_Resolve_Type (Ctx, B);
         if B = Cur then
            if KIND (Cur) in DN_ENUMERATION | DN_PRIVATE | DN_L_PRIVATE
              and then not Is_Void_Tree (D (SM_DERIVED, Cur))
            then
               declare
                  Der : constant TREE := Subst_Resolve_Type (Ctx, D (SM_DERIVED, Cur));
               begin
                  if not Is_Void_Tree (Der) and then Der /= Cur then
                     Cur := Der;
                     goto Next_Step;
                  end if;
               end;
            end if;
            exit;
         end if;
         Cur := B;
         <<Next_Step>>
      end loop;
      return Cur;
   end Root_Base_Type;

   function Subst_Base_Type (Ctx : Context; T : TREE) return TREE is
   begin
      return Root_Base_Type (Ctx, T);
   end Subst_Base_Type;

   procedure Bind_Temp_Var
     (Ctx   : in out Context;
      Scope : in out Temp_Var_Scope;
      Key   : Unsigned_32;
      Val   : Var_Info)
   is
      Entry_S : Saved_Var_Entry;
   begin
      Entry_S.Key := Key;
      if Ctx.Vars.Contains (Key) then
         Entry_S.Had_Prev := True;
         Entry_S.Prev_Var := Ctx.Vars.Element (Key);
      end if;
      Ctx.Vars.Include (Key, Val);
      Scope.Append (Entry_S);
   end Bind_Temp_Var;

   procedure Restore_Temp_Vars
     (Ctx   : in out Context;
      Scope : in out Temp_Var_Scope)
   is
   begin
      for I in reverse 0 .. Integer (Scope.Length) - 1 loop
         declare
            SB : constant Saved_Var_Entry := Scope.Element (I);
         begin
            if SB.Had_Prev then
               Ctx.Vars.Include (SB.Key, SB.Prev_Var);
            elsif Ctx.Vars.Contains (SB.Key) then
               Ctx.Vars.Delete (SB.Key);
            end if;
         end;
      end loop;
      Scope.Clear;
   end Restore_Temp_Vars;

   function Save_Subp_State (Ctx : Context) return Subp_State_Snapshot is
   begin
      return
        (Cur_Subp_Key                => Ctx.Cur_Subp_Key,
         Cur_Subp_Node               => Ctx.Cur_Subp_Node,
         Cur_Ret_Type                => Ctx.Cur_Ret_Type,
         Cur_Ret_Comp                => Ctx.Cur_Ret_Comp,
         Cur_Ret_Uncon               => Ctx.Cur_Ret_Uncon,
         Cur_Ret_Ptr                 => Ctx.Cur_Ret_Ptr,
         Cur_Frame_Reg               => Ctx.Cur_Frame_Reg,
         Cur_SL_Reg                  => Ctx.Cur_SL_Reg,
         Cur_Master_Reg              => Ctx.Cur_Master_Reg,
         Cur_Task_Reg                => Ctx.Cur_Task_Reg,
         Cur_Task_Exit_Lbl           => Ctx.Cur_Task_Exit_Lbl,
         Cur_Task_Base_Cleanup_Depth => Ctx.Cur_Task_Base_Cleanup_Depth,
         Cur_Is_Func                 => Ctx.Cur_Is_Func,
         Loops                       => Ctx.Loops,
         Handlers                    => Ctx.Handlers,
         Copy_Outs                   => Ctx.Copy_Outs,
         Cleanups                    => Ctx.Cleanups,
         Accept_Frames               => Ctx.Accept_Frames,
         Label_Cleanup_Depths        => Ctx.Label_Cleanup_Depths,
         Uplevel_Cache               => Ctx.Uplevel_Cache);
   end Save_Subp_State;

   procedure Reset_Subp_State (Ctx : in out Context) is
   begin
      Ctx.Cur_Frame_Reg               := Null_Unbounded_String;
      Ctx.Cur_Master_Reg              := Null_Unbounded_String;
      Ctx.Cur_Task_Reg                := Null_Unbounded_String;
      Ctx.Cur_Task_Exit_Lbl           := Null_Unbounded_String;
      Ctx.Cur_Task_Base_Cleanup_Depth := 0;
      Ctx.Loops.Clear;
      Ctx.Handlers.Clear;
      Ctx.Copy_Outs.Clear;
      Ctx.Cleanups.Clear;
      Ctx.Accept_Frames.Clear;
      Ctx.Label_Cleanup_Depths.Clear;
      Ctx.Uplevel_Cache.Clear;
   end Reset_Subp_State;

   procedure Restore_Subp_State (Ctx : in out Context; Snap : Subp_State_Snapshot) is
   begin
      Ctx.Cur_Subp_Key                := Snap.Cur_Subp_Key;
      Ctx.Cur_Subp_Node               := Snap.Cur_Subp_Node;
      Ctx.Cur_Ret_Type                := Snap.Cur_Ret_Type;
      Ctx.Cur_Ret_Comp                := Snap.Cur_Ret_Comp;
      Ctx.Cur_Ret_Uncon               := Snap.Cur_Ret_Uncon;
      Ctx.Cur_Ret_Ptr                 := Snap.Cur_Ret_Ptr;
      Ctx.Cur_Frame_Reg               := Snap.Cur_Frame_Reg;
      Ctx.Cur_SL_Reg                  := Snap.Cur_SL_Reg;
      Ctx.Cur_Master_Reg              := Snap.Cur_Master_Reg;
      Ctx.Cur_Task_Reg                := Snap.Cur_Task_Reg;
      Ctx.Cur_Task_Exit_Lbl           := Snap.Cur_Task_Exit_Lbl;
      Ctx.Cur_Task_Base_Cleanup_Depth := Snap.Cur_Task_Base_Cleanup_Depth;
      Ctx.Cur_Is_Func                 := Snap.Cur_Is_Func;
      Ctx.Loops                       := Snap.Loops;
      Ctx.Handlers                    := Snap.Handlers;
      Ctx.Copy_Outs                   := Snap.Copy_Outs;
      Ctx.Cleanups                    := Snap.Cleanups;
      Ctx.Accept_Frames               := Snap.Accept_Frames;
      Ctx.Label_Cleanup_Depths        := Snap.Label_Cleanup_Depths;
      Ctx.Uplevel_Cache               := Snap.Uplevel_Cache;
   end Restore_Subp_State;

   function Emit_Elem_Ptr
     (Ctx      : in out Context;
      ETI      : Type_Info;
      Base_Ptr : String;
      Idx_64   : String) return String
   is
      EP : constant String := Next_Reg (Ctx.E, "ep");
   begin
      if ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic then
         Emit_Inst (Ctx.E, EP & " = elem_ptr @__ada_arr_desc, " & Base_Ptr & ", " & Idx_64);
      elsif ETI.Kind = TK_Constrained_Array and then not ETI.Is_Dynamic then
         declare
            Al     : constant Natural := Natural'Max (1, ETI.Align);
            Stride : constant Natural := Natural'Max (1, ((ETI.Byte_Size + Al - 1) / Al) * Al);
            BOff   : constant String := Next_Reg (Ctx.E, "ebo");
         begin
            Emit_Inst (Ctx.E, BOff & " = mul.i64 " & Idx_64 & ", " & Img (Stride));
            Emit_Inst (Ctx.E, EP & " = ptr_add.i64 " & Base_Ptr & ", " & BOff);
         end;
      else
         Emit_Inst (Ctx.E, EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & Base_Ptr & ", " & Idx_64);
      end if;
      return EP;
   end Emit_Elem_Ptr;

   procedure Ensure_Runtime_Externs (Ctx : in out Context) is
      Arr_Desc_Decl : Unbounded_String := To_Unbounded_String ("type @__ada_arr_desc = { ptr");
   begin
      for D in 1 .. Max_Array_Dims loop
         Append (Arr_Desc_Decl, ", i64, i64");
      end loop;
      Append (Arr_Desc_Decl, " }");
      Add_Type_Decl (Ctx.E, To_String (Arr_Desc_Decl));
      Add_Extern_Func (Ctx.E, "@__ada_raise",
        "extern func @__ada_raise(%tag: ptr) -> void cc ""c"" noreturn");
      Add_Extern_Func (Ctx.E, "@__ada_unhandled_exception",
        "extern func @__ada_unhandled_exception(%tag: ptr) -> void cc ""c"" noreturn");
      Add_Extern_Func (Ctx.E, "@__ada_clock_ns",
        "extern func @__ada_clock_ns() -> i64 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_cal_time_of",
        "extern func @__ada_cal_time_of(%y: i32, %m: i32, %d: i32, %s: i64) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cal_split",
        "extern func @__ada_cal_split(%t: i64, %y: ptr, %m: ptr, %d: ptr, %s: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cal_year",
        "extern func @__ada_cal_year(%t: i64) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cal_month",
        "extern func @__ada_cal_month(%t: i64) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cal_day",
        "extern func @__ada_cal_day(%t: i64) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cal_seconds",
        "extern func @__ada_cal_seconds(%t: i64) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cstr_len",
        "extern func @__ada_cstr_len(%s: ptr) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_img_i64",
        "extern func @__ada_img_i64(%v: i64, %buf: ptr) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_img_char",
        "extern func @__ada_img_char(%c: i8, %buf: ptr) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_val_i64",
        "extern func @__ada_val_i64(%p: ptr, %n: i32) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_val_char",
        "extern func @__ada_val_char(%p: ptr, %n: i32) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_val_enum",
        "extern func @__ada_val_enum(%p: ptr, %n: i32, %tbl: ptr, %cnt: i32) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_ipow_i32",
        "extern func @__ada_ipow_i32(%b: i32, %e: i32, %chk: i32) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_ipow_i64",
        "extern func @__ada_ipow_i64(%b: i64, %e: i32, %chk: i32) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_fpow_f32",
        "extern func @__ada_fpow_f32(%b: f32, %e: i32) -> f32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_fpow_f64",
        "extern func @__ada_fpow_f64(%b: f64, %e: i32) -> f64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_f64_to_i64_round",
        "extern func @__ada_f64_to_i64_round(%x: f64, %lo: i64, %hi: i64, %chk: i32) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_alloc",
        "extern func @__ada_alloc(%sz: i64) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_pool_alloc",
        "extern func @__ada_pool_alloc(%up: ptr, %lim: i64, %sz: i64) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_pool_free",
        "extern func @__ada_pool_free(%up: ptr, %p: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_free",
        "extern func @__ada_free(%p: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_ss_mark",
        "extern func @__ada_ss_mark() -> i64 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_ss_alloc",
        "extern func @__ada_ss_alloc(%sz: i64, %al: i64) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_ss_release",
        "extern func @__ada_ss_release(%mark: i64) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_int_width",
        "extern func @__ada_int_width(%lo: i64, %hi: i64) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_enum_width",
        "extern func @__ada_enum_width(%tbl: ptr, %cnt: i32, %lo: i64, %hi: i64) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_fixed_mantissa",
        "extern func @__ada_fixed_mantissa(%lo: i64, %hi: i64, %bs: f64, %ss: f64) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_fixed_large_ticks",
        "extern func @__ada_fixed_large_ticks(%lo: i64, %hi: i64, %bs: f64, %ss: f64) -> i64 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_fixed_fore",
        "extern func @__ada_fixed_fore(%lo: i64, %hi: i64, %bs: f64) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_fixed_fore_rat",
        "extern func @__ada_fixed_fore_rat(%lo: i64, %hi: i64, %sn: i64, %sd: i64) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_fixed_mul_i64",
        "extern func @__ada_fixed_mul_i64(%a: i64, %b: i64, %sn: i64, %sd: i64, %lo: i64, %hi: i64, %chk: i32) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_fixed_div_i64",
        "extern func @__ada_fixed_div_i64(%a: i64, %b: i64, %sn: i64, %sd: i64, %lo: i64, %hi: i64, %chk: i32) -> i64 cc ""c""");
      -- Chapter 14 I/O runtime (ada_io.c)
      Add_Extern_Func (Ctx.E, "@__ada_tio_create",
        "extern func @__ada_tio_create(%f: ptr, %m: i32, %np: ptr, %nl: i32, %fp: ptr, %fl: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_open",
        "extern func @__ada_tio_open(%f: ptr, %m: i32, %np: ptr, %nl: i32, %fp: ptr, %fl: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_close",
        "extern func @__ada_tio_close(%f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_delete",
        "extern func @__ada_tio_delete(%f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_reset",
        "extern func @__ada_tio_reset(%f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_reset_mode",
        "extern func @__ada_tio_reset_mode(%f: ptr, %m: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_is_open",
        "extern func @__ada_tio_is_open(%f: ptr) -> i8 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_tio_mode",
        "extern func @__ada_tio_mode(%f: ptr) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_name",
        "extern func @__ada_tio_name(%f: ptr, %len: ptr) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_form",
        "extern func @__ada_tio_form(%f: ptr, %len: ptr) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_standard_input",
        "extern func @__ada_tio_standard_input() -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_tio_standard_output",
        "extern func @__ada_tio_standard_output() -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_tio_current_input",
        "extern func @__ada_tio_current_input() -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_tio_current_output",
        "extern func @__ada_tio_current_output() -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_tio_set_input",
        "extern func @__ada_tio_set_input(%f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_set_output",
        "extern func @__ada_tio_set_output(%f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_set_line_length",
        "extern func @__ada_tio_set_line_length(%f: ptr, %to: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_set_page_length",
        "extern func @__ada_tio_set_page_length(%f: ptr, %to: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_line_length",
        "extern func @__ada_tio_line_length(%f: ptr) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_page_length",
        "extern func @__ada_tio_page_length(%f: ptr) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_col",
        "extern func @__ada_tio_col(%f: ptr) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_line",
        "extern func @__ada_tio_line(%f: ptr) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_page",
        "extern func @__ada_tio_page(%f: ptr) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_new_line",
        "extern func @__ada_tio_new_line(%f: ptr, %sp: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_new_page",
        "extern func @__ada_tio_new_page(%f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_skip_line",
        "extern func @__ada_tio_skip_line(%f: ptr, %sp: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_skip_page",
        "extern func @__ada_tio_skip_page(%f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_end_of_line",
        "extern func @__ada_tio_end_of_line(%f: ptr) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_end_of_page",
        "extern func @__ada_tio_end_of_page(%f: ptr) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_end_of_file",
        "extern func @__ada_tio_end_of_file(%f: ptr) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_set_col",
        "extern func @__ada_tio_set_col(%f: ptr, %to: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_set_line",
        "extern func @__ada_tio_set_line(%f: ptr, %to: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_char",
        "extern func @__ada_tio_put_char(%f: ptr, %c: i8) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_str",
        "extern func @__ada_tio_put_str(%f: ptr, %p: ptr, %n: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_line",
        "extern func @__ada_tio_put_line(%f: ptr, %p: ptr, %n: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_char",
        "extern func @__ada_tio_get_char(%f: ptr) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_str",
        "extern func @__ada_tio_get_str(%f: ptr, %p: ptr, %n: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_line",
        "extern func @__ada_tio_get_line(%f: ptr, %p: ptr, %fi: i32, %n: i32, %lo: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_i64",
        "extern func @__ada_tio_put_i64(%f: ptr, %v: i64, %w: i32, %b: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_i64_str",
        "extern func @__ada_tio_put_i64_str(%d: ptr, %n: i32, %v: i64, %b: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_i64",
        "extern func @__ada_tio_get_i64(%f: ptr, %w: i32) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_raise_data_error",
        "extern func @__ada_raise_data_error() -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_i64_str",
        "extern func @__ada_tio_get_i64_str(%s: ptr, %fi: i32, %n: i32, %lo: ptr) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_f64",
        "extern func @__ada_tio_put_f64(%f: ptr, %v: f64, %fr: i32, %af: i32, %ex: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_f64_str",
        "extern func @__ada_tio_put_f64_str(%d: ptr, %n: i32, %v: f64, %af: i32, %ex: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_f64",
        "extern func @__ada_tio_get_f64(%f: ptr, %w: i32) -> f64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_f64_str",
        "extern func @__ada_tio_get_f64_str(%s: ptr, %fi: i32, %n: i32, %lo: ptr) -> f64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_enum",
        "extern func @__ada_tio_put_enum(%f: ptr, %lit: ptr, %w: i32, %lc: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_put_enum_str",
        "extern func @__ada_tio_put_enum_str(%d: ptr, %n: i32, %lit: ptr, %lc: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_enum",
        "extern func @__ada_tio_get_enum(%f: ptr, %tbl: ptr, %cnt: i32, %isch: i32) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_tio_get_enum_str",
        "extern func @__ada_tio_get_enum_str(%s: ptr, %fi: i32, %n: i32, %tbl: ptr, %cnt: i32, %lo: ptr, %isch: i32) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_bio_create",
        "extern func @__ada_bio_create(%f: ptr, %k: i32, %m: i32, %np: ptr, %nl: i32, %fp: ptr, %fl: i32, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_bio_open",
        "extern func @__ada_bio_open(%f: ptr, %k: i32, %m: i32, %np: ptr, %nl: i32, %fp: ptr, %fl: i32, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_seq_read",
        "extern func @__ada_seq_read(%f: ptr, %it: ptr, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_seq_write",
        "extern func @__ada_seq_write(%f: ptr, %it: ptr, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_seq_read_framed",
        "extern func @__ada_seq_read_framed(%f: ptr, %it: ptr, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_seq_write_framed",
        "extern func @__ada_seq_write_framed(%f: ptr, %it: ptr, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_seq_read_framed_nd",
        "extern func @__ada_seq_read_framed_nd(%f: ptr, %it: ptr, %nd: i32, %lens: ptr, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_seq_write_framed_nd",
        "extern func @__ada_seq_write_framed_nd(%f: ptr, %it: ptr, %nd: i32, %lens: ptr, %esz: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_seq_eof",
        "extern func @__ada_seq_eof(%f: ptr) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_dir_read",
        "extern func @__ada_dir_read(%f: ptr, %it: ptr, %esz: i64, %fr: i64, %hp: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_dir_write",
        "extern func @__ada_dir_write(%f: ptr, %it: ptr, %esz: i64, %to: i64, %hp: i32) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_llio_send",
        "extern func @__ada_llio_send(%dev: i32, %dat: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_llio_recv",
        "extern func @__ada_llio_recv(%dev: i32, %dat: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_dir_set_index",
        "extern func @__ada_dir_set_index(%f: ptr, %to: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_dir_index",
        "extern func @__ada_dir_index(%f: ptr) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_dir_size",
        "extern func @__ada_dir_size(%f: ptr) -> i32 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_dir_eof",
        "extern func @__ada_dir_eof(%f: ptr) -> i8 cc ""c""");
      -- Chapter 9 Tasking Runtime (ada_task.c)
      Add_Extern_Func (Ctx.E, "@__ada_global_master",
        "extern func @__ada_global_master() -> ptr cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_self",
        "extern func @__ada_task_self() -> ptr cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_master_enter",
        "extern func @__ada_master_enter() -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_master_activate",
        "extern func @__ada_master_activate(%m: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_master_leave",
        "extern func @__ada_master_leave(%m: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_master_abort_unactivated",
        "extern func @__ada_master_abort_unactivated(%m: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_master_mark",
        "extern func @__ada_master_mark(%m: ptr) -> ptr cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_master_activate_since",
        "extern func @__ada_master_activate_since(%m: ptr, %mk: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_master_abort_since",
        "extern func @__ada_master_abort_since(%m: ptr, %mk: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_cal_add",
        "extern func @__ada_cal_add(%t: i64, %d: i64) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cal_sub_dur",
        "extern func @__ada_cal_sub_dur(%t: i64, %d: i64) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_cal_sub_time",
        "extern func @__ada_cal_sub_time(%t1: i64, %t2: i64) -> i64 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_task_create",
        "extern func @__ada_task_create(%m: ptr, %fn: ptr, %env: ptr, %ne: i32) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_task_create_and_activate",
        "extern func @__ada_task_create_and_activate(%m: ptr, %fn: ptr, %env: ptr, %ne: i32) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_task_set_elab_flag",
        "extern func @__ada_task_set_elab_flag(%t: ptr, %f: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_task_set_group",
        "extern func @__ada_task_set_group(%t: ptr, %gid: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_master_activate_group",
        "extern func @__ada_master_activate_group(%m: ptr, %gid: i64) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_task_activated",
        "extern func @__ada_task_activated(%t: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_activation_failed",
        "extern func @__ada_task_activation_failed(%t: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_abort",
        "extern func @__ada_task_abort(%t: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_abort_list",
        "extern func @__ada_task_abort_list(%ts: ptr, %n: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_check_abort",
        "extern func @__ada_check_abort() -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_set_priority",
        "extern func @__ada_task_set_priority(%t: ptr, %p: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_set_main_priority",
        "extern func @__ada_set_main_priority(%p: i32) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_get_active_master",
        "extern func @__ada_get_active_master() -> ptr cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_set_active_master",
        "extern func @__ada_set_active_master(%m: ptr) -> ptr cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_set_stack_size",
        "extern func @__ada_task_set_stack_size(%t: ptr, %sz: i64) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_completed",
        "extern func @__ada_task_completed(%t: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_callable",
        "extern func @__ada_task_callable(%t: ptr) -> i8 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_task_terminated",
        "extern func @__ada_task_terminated(%t: ptr) -> i8 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_entry_count",
        "extern func @__ada_entry_count(%t: ptr, %ei: i32) -> i32 cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_delay",
        "extern func @__ada_delay(%ns: i64) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_entry_call",
        "extern func @__ada_entry_call(%t: ptr, %ei: i32, %pkt: ptr) -> void cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_entry_call_cond",
        "extern func @__ada_entry_call_cond(%t: ptr, %ei: i32, %pkt: ptr) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_entry_call_timed",
        "extern func @__ada_entry_call_timed(%t: ptr, %ei: i32, %pkt: ptr, %ns: i64) -> i8 cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_accept_begin",
        "extern func @__ada_accept_begin(%t: ptr, %ei: i32, %pout: ptr) -> ptr cc ""c""");
      Add_Extern_Func (Ctx.E, "@__ada_accept_end",
        "extern func @__ada_accept_end(%t: ptr, %c: ptr, %ex: ptr) -> void cc ""c"" nounwind");
      Add_Extern_Func (Ctx.E, "@__ada_select_wait",
        "extern func @__ada_select_wait(%t: ptr, %na: i32, %ak: ptr, %ei: ptr, %dl: ptr, %he: i32, %po: ptr, %co: ptr) -> i32 cc ""c""");
   end Ensure_Runtime_Externs;

   procedure Register_Task_Spec
     (Ctx          : in out Context;
      Task_Spec    : TREE;
      Parent_Subp  : Unsigned_32;
      Force_Rebind : Boolean := False)
   is
      TS  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Task_Spec));
      TK  : Unsigned_32;
      TI  : Task_Info;
      Next_Slot : Natural := 0;
   begin
      if Is_Void_Tree (TS) or else KIND (TS) /= DN_TASK_SPEC then
         return;
      end if;
      TK := Tree_Key (TS);
      if Ctx.Tasks.Contains (TK) then
         if not Force_Rebind then
            return;
         end if;
         if Length (Ctx.Tasks.Element (TK).Inst_Tag) = 0
           or else Ctx.Tasks.Element (TK).Inst_Tag = Ctx.Gen_Inst_Tag
         then
            return;
         end if;
      end if;
      -- Follow SM_DERIVED to root task spec so derived task types share their parent's task body
      declare
         Root_TS : TREE := TS;
      begin
         for Step in 1 .. 16 loop
            exit when Is_Void_Tree (D (SM_DERIVED, Root_TS));
            declare
               Par : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DERIVED, Root_TS)));
            begin
               exit when Is_Void_Tree (Par) or else KIND (Par) /= DN_TASK_SPEC or else Par = Root_TS;
               Root_TS := Par;
            end;
         end loop;
          if Root_TS /= TS then
             Register_Task_Spec (Ctx, Root_TS, Parent_Subp, Force_Rebind => False);
             declare
                Root_TI : constant Task_Info := Ctx.Tasks.Element (Tree_Key (Root_TS));
             begin
                TI.Body_Fn          := Root_TI.Body_Fn;
                TI.Parent_Subp      := Root_TI.Parent_Subp;
                TI.Needs_SL         := Root_TI.Needs_SL;
                TI.Elab_Flag        := Root_TI.Elab_Flag;
                TI.Elab_Is_Local    := Root_TI.Elab_Is_Local;
                TI.Elab_Owner       := Root_TI.Elab_Owner;
                TI.Elab_Cap_Slot    := Root_TI.Elab_Cap_Slot;
                TI.Elab_Fallback    := Root_TI.Elab_Fallback;
                TI.Has_Priority     := Root_TI.Has_Priority;
                TI.Priority         := Root_TI.Priority;
                TI.Storage_Size     := Root_TI.Storage_Size;
                TI.Storage_Size_Exp := Root_TI.Storage_Size_Exp;
                TI.Has_Dyn_Entries  := Root_TI.Has_Dyn_Entries;
                TI.Dyn_Entries_Sym  := Root_TI.Dyn_Entries_Sym;
                TI.Dyn_Entries_Exp  := Root_TI.Dyn_Entries_Exp;
             end;
          else
             declare
                Src : constant TREE := D (XD_SOURCE_NAME, TS);
                Nm  : constant String := (if not Is_Void_Tree (Src) then Qualified_Ada_Name (Src) else "task");
                Is_Sep_Task : constant Boolean :=
                  Length (Ctx.Gen_Inst_Tag) = 0 and then not Ctx.In_Gen_Body
                  and then (Ctx.Has_Stubs or else Ctx.In_Subunit or else not Is_Void_Tree (D (XD_STUB, TS)));
             begin
                if not Is_Void_Tree (Src) and then (Is_Library_Level (Src) or else Is_Sep_Task) then
                   TI.Body_Fn := To_Unbounded_String ("@ada_task__" & Nm);
                   if Is_Library_Level (Src) and then Ctx.Enable_Checks then
                      TI.Elab_Flag := To_Unbounded_String ("@ada_telab__" & Nm);
                   end if;
                else
                   TI.Body_Fn := To_Unbounded_String ("@ada_task__" & Nm & "_" & Tree_Key_Str (TS) & To_String (Ctx.Gen_Inst_Tag));
                end if;
                TI.Parent_Subp := Parent_Subp;
             end;
          end if;
      end;
      -- Walk SM_DECL_S for entries and entry families
      declare
         DS : constant TREE := D (SM_DECL_S, TS);
      begin
         if not Is_Void_Tree (DS) then
            declare
               DL : SEQ_TYPE := LIST (DS);
               It : TREE;
            begin
               while not IS_EMPTY (DL) loop
                  POP (DL, It);
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
                           begin
                              if KIND (A1) = DN_ASSOC then
                                 A1 := D (AS_EXP, A1);
                              end if;
                              SV := Safe_SM_Value (A1);
                              if Has_Static_Val (SV) and then KIND (SV) = DN_NUM_VAL then
                                 declare
                                    PV : constant Integer_64 := Eval_Int64 (SV);
                                 begin
                                    if PV >= 0 and then PV <= 10 then
                                       TI.Has_Priority := True;
                                       TI.Priority := Integer (PV);
                                    end if;
                                 end;
                              end if;
                           end;
                        end if;
                     end;
                  elsif KIND (It) = DN_SUBPROG_ENTRY_DECL then
                     declare
                        EID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
                        Hdr : constant TREE := D (AS_HEADER, It);
                        EK  : constant Unsigned_32 := Tree_Key (EID);
                        EI  : Entry_Info;
                     begin
                        EI.Task_Key     := TK;
                        EI.Base_Idx     := Next_Slot;
                        EI.Entry_Header := Hdr;
                        -- P3-6.10: Record interrupt entry address clause if present (LRM 13.5.1)
                        if not Is_Void_Tree (D (SM_ADDRESS, EID)) then
                           EI.Has_Address := True;
                           EI.Addr_Exp    := D (SM_ADDRESS, EID);
                        end if;
                        -- Check for entry family discrete range
                        if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_ENTRY
                          and then not Is_Void_Tree (D (AS_DISCRETE_RANGE, Hdr))
                        then
                           declare
                              DR : constant TREE := D (AS_DISCRETE_RANGE, Hdr);
                              St : Boolean := False;
                              Lo, Hi : Integer_64 := 0;
                           begin
                              EI.Is_Family := True;
                              EI.Fam_DR    := DR;
                              if KIND (DR) = DN_RANGE then
                                 declare
                                    E1 : constant TREE := Safe_SM_Value (D (AS_EXP1, DR));
                                    E2 : constant TREE := Safe_SM_Value (D (AS_EXP2, DR));
                                 begin
                                    if Has_Static_Val (E1) and then Has_Static_Val (E2) then
                                       St := True;
                                       Lo := Eval_Int64 (E1);
                                       Hi := Eval_Int64 (E2);
                                    end if;
                                 end;
                              elsif KIND (DR) = DN_DISCRETE_SUBTYPE then
                                 declare
                                    SI_N : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                                    C_N  : constant TREE := (if KIND (SI_N) = DN_SUBTYPE_INDICATION then D (AS_CONSTRAINT, SI_N) else CONST_VOID);
                                 begin
                                    if not Is_Void_Tree (C_N) and then KIND (C_N) = DN_RANGE then
                                       declare
                                          E1 : constant TREE := Safe_SM_Value (D (AS_EXP1, C_N));
                                          E2 : constant TREE := Safe_SM_Value (D (AS_EXP2, C_N));
                                       begin
                                          if Has_Static_Val (E1) and then Has_Static_Val (E2) then
                                             St := True;
                                             Lo := Eval_Int64 (E1);
                                             Hi := Eval_Int64 (E2);
                                          end if;
                                       end;
                                    elsif not Is_Void_Tree (C_N) and then KIND (C_N) = DN_RANGE_ATTRIBUTE then
                                       St := False;
                                    elsif KIND (SI_N) = DN_SUBTYPE_INDICATION then
                                       Get_Discrete_Bounds (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, SI_N)))), St, Lo, Hi);
                                    else
                                       Get_Discrete_Bounds (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (SI_N))), St, Lo, Hi);
                                    end if;
                                 end;
                              elsif KIND (DR) = DN_RANGE_ATTRIBUTE then
                                 St := False;
                              end if;
                              EI.Fam_Is_Static := St;
                              if St and then Hi < Lo then
                                 EI.Fam_Lo := Lo;
                                 EI.Fam_Hi := Hi;
                              elsif St and then Hi >= Lo
                                and then (Unsigned_64'Mod (Hi) - Unsigned_64'Mod (Lo)) < 1_000_000
                              then
                                 EI.Fam_Lo := Lo;
                                 EI.Fam_Hi := Hi;
                                 Next_Slot := Next_Slot + Natural (Unsigned_64'Mod (Hi) - Unsigned_64'Mod (Lo) + 1);
                              else
                                 EI.Fam_Is_Static := False;
                                 TI.Has_Dyn_Entries := True;
                                 EI.Fam_Lo := 0;
                                 EI.Fam_Hi := 63;
                                 Next_Slot := Next_Slot + 64;
                              end if;
                           end;
                        else
                           Next_Slot := Next_Slot + 1;
                        end if;
                        if TI.Has_Dyn_Entries then
                           declare
                              Src_T  : constant TREE := D (XD_SOURCE_NAME, TS);
                              Is_Lib : constant Boolean := not Is_Void_Tree (Src_T) and then Is_Library_Level (Src_T);
                              EQName : constant String := Qualified_Ada_Name (EID) & "__" & Img (EI.Base_Idx);
                           begin
                              if Is_Lib then
                                 EI.Fam_Exported := True;
                                 EI.Fam_Base_Sym := To_Unbounded_String ("@ada_efb__" & EQName);
                                 if EI.Is_Family and then not EI.Fam_Is_Static then
                                    EI.Fam_Lo_Sym := To_Unbounded_String ("@ada_efl__" & EQName);
                                    EI.Fam_Hi_Sym := To_Unbounded_String ("@ada_efh__" & EQName);
                                 end if;
                                 if Length (TI.Dyn_Entries_Sym) = 0 then
                                    TI.Dyn_Entries_Sym := To_Unbounded_String ("@ada_efn__" & Qualified_Ada_Name (Src_T));
                                    TI.Dyn_Entries_Exp := True;
                                 end if;
                              else
                                 EI.Fam_Base_Sym := To_Unbounded_String ("@ada_efb_" & Tree_Key_Str (EID) & To_String (Ctx.Gen_Inst_Tag));
                                 if EI.Is_Family and then not EI.Fam_Is_Static then
                                    EI.Fam_Lo_Sym := To_Unbounded_String ("@ada_efl_" & Tree_Key_Str (EID) & To_String (Ctx.Gen_Inst_Tag));
                                    EI.Fam_Hi_Sym := To_Unbounded_String ("@ada_efh_" & Tree_Key_Str (EID) & To_String (Ctx.Gen_Inst_Tag));
                                 end if;
                                 if Length (TI.Dyn_Entries_Sym) = 0 then
                                    TI.Dyn_Entries_Sym := To_Unbounded_String ("@ada_efn_" & Tree_Key_Str (TS) & To_String (Ctx.Gen_Inst_Tag));
                                 end if;
                              end if;
                           end;
                        end if;

                        -- Build PIR parameter packet struct type for entry formal parameters
                        if not Is_Void_Tree (Hdr) and then not Is_Void_Tree (D (AS_PARAM_S, Hdr)) then
                           declare
                              PL     : SEQ_TYPE := LIST (D (AS_PARAM_S, Hdr));
                              P      : TREE;
                              Fields : Unbounded_String;
                              FCnt   : Natural := 0;
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
                                          PT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                                          PTI : constant Type_Info := Get_Type_Info (Ctx, PT);
                                       begin
                                          if FCnt > 0 then Append (Fields, ", "); end if;
                                          if PTI.Kind = TK_Unconstrained_Array
                                            or else (PTI.Kind = TK_Constrained_Array and then PTI.Is_Dynamic)
                                          then
                                             Append (Fields, "ptr");
                                             for D_I in 1 .. Natural'Max (1, Natural'Min (PTI.NDims, Max_Array_Dims)) loop
                                                Append (Fields, ", i64, i64");
                                             end loop;
                                          elsif PTI.Kind /= TK_Scalar then
                                             Append (Fields, "ptr");
                                             if Needs_Constrained_Param (Ctx, PT, Mode) then
                                                Append (Fields, ", i8");
                                             end if;
                                          else
                                             -- Copy-in / copy-out value slot in packet for ALL scalar params (in, out, in out)
                                             Append (Fields, To_String (PTI.Pir_Type));
                                          end if;
                                          FCnt := FCnt + 1;
                                       end;
                                    end loop;
                                 end;
                              end loop;
                              if FCnt > 0 then
                                 declare
                                    PSym : constant String := "@__ada_ep_" & Tree_Key_Str (EID) & To_String (Ctx.Gen_Inst_Tag);
                                 begin
                                    Add_Type_Decl (Ctx.E, "type " & PSym & " = { " & To_String (Fields) & " }");
                                    EI.Pkt_Type   := To_Unbounded_String (PSym);
                                    EI.Has_Params := True;
                                 end;
                              end if;
                           end;
                        end if;
                        Ctx.Entries.Include (EK, EI);
                     end;
                  end if;
               end loop;
            end;
         end if;
      end;
      TI.Num_Entries := Natural'Max (1, Next_Slot);
      declare
         SS_E : TREE := D (SM_STORAGE_SIZE, TS);
      begin
         if not Is_Void_Tree (SS_E) then
            declare
               SV : constant TREE := Safe_SM_Value (SS_E);
            begin
               if Has_Static_Val (SV) then
                  TI.Storage_Size := Eval_Int64 (SV);
               else
                  TI.Storage_Size_Exp := SS_E;
               end if;
            end;
         end if;
      end;
      TI.Inst_Tag    := Ctx.Gen_Inst_Tag;
      Ctx.Tasks.Include (TK, TI);
   end Register_Task_Spec;

   procedure Walk_Static_Link
     (Ctx          : in out Context;
      Target_Owner : Unsigned_32;
      Hoist_Entry  : Boolean;
      Found        : out Boolean;
      Frame_Ptr    : out Unbounded_String;
      Frame_Type   : out Unbounded_String);

   function Emit_Enclosing_Task_Self_By_Key
     (Ctx       : in out Context;
      Target_TK : Unsigned_32) return String
   is
   begin
      if Target_TK /= 0 and then Target_TK = Ctx.Cur_Subp_Key and then Length (Ctx.Cur_Task_Reg) > 0 then
         return To_String (Ctx.Cur_Task_Reg);
      end if;
      if Target_TK /= 0 and then Length (Ctx.Cur_SL_Reg) > 0 then
         declare
            Found       : Boolean;
            F_Ptr, F_Ty : Unbounded_String;
         begin
            Walk_Static_Link (Ctx, Target_TK, Hoist_Entry => False, Found => Found, Frame_Ptr => F_Ptr, Frame_Type => F_Ty);
            if Found then
               declare
                  SP : constant String := Next_Reg (Ctx.E, "etsp");
                  SV : constant String := Next_Reg (Ctx.E, "etsv");
               begin
                  Emit_Inst (Ctx.E, SP & " = field_ptr " & To_String (F_Ty) & ", " & To_String (F_Ptr) & ", 1");
                  Emit_Inst (Ctx.E, SV & " = load.ptr " & SP & ", align 8");
                  return SV;
               end;
            end if;
         end;
      end if;
      if Length (Ctx.Cur_Task_Reg) > 0 and then Target_TK = 0 then
         return To_String (Ctx.Cur_Task_Reg);
      end if;
      declare
         R : constant String := Next_Reg (Ctx.E, "tself");
      begin
         Emit_Inst (Ctx.E, R & " = call @__ada_task_self()");
         return R;
      end;
   end Emit_Enclosing_Task_Self_By_Key;

   function Get_Arr_Desc_Type (Ctx : in out Context; NDims : Natural) return String is
      ND   : constant Natural := (if NDims < 1 then 1 elsif NDims > Max_Array_Dims then Max_Array_Dims else NDims);
      Name : constant String := "@__ada_arr_desc_" & Img (ND);
      Decl : Unbounded_String := To_Unbounded_String ("type " & Name & " = { ptr");
   begin
      for I in 1 .. ND loop
         Append (Decl, ", i64, i64");
      end loop;
      Append (Decl, " }");
      Add_Type_Decl (Ctx.E, To_String (Decl));
      return Name;
   end Get_Arr_Desc_Type;

   function Elem_Storage_Size (ETI : Type_Info) return Natural is
      EAl : Natural;
   begin
      if ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic then
         return Arr_Desc_Size;
      end if;
      EAl := Natural'Max (1, ETI.Align);
      return Natural'Max (1, ((ETI.Byte_Size + EAl - 1) / EAl) * EAl);
   end Elem_Storage_Size;

   function Elem_Storage_Align (ETI : Type_Info) return Natural is
   begin
      if ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic then
         return 8;
      end if;
      return Natural'Max (1, ETI.Align);
   end Elem_Storage_Align;

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
      Can  : constant TREE := Canonical_Exc (Subst_Tree (Ctx, Canonical_Exc (Subst_Tree (Ctx, Exc_Id))));
      Key  : constant Unsigned_32 := Tree_Key (Can);
      Name : constant String := Sym_Name (Can);
      Pkg  : constant String := Enclosing_Package_Name (Can);
   begin
      if Is_Void_Tree (Can) then
         return Constraint_Error_Tag (Ctx);
      end if;
      if Ctx.Gen_Local_Exc_Tags.Contains (Key) then
         return To_String (Ctx.Gen_Local_Exc_Tags.Element (Key));
      end if;
      -- Standard / predefined library exceptions (must be inside predefined STANDARD, not user PACKAGE STANDARD! c86003a)
      if ((Pkg = "STANDARD" and then KIND (Can) in CLASS_SOURCE_NAME
             and then Is_Predefined_Standard_Pkg (D (XD_REGION, Can)))
          or else (Pkg = "" and then Is_Library_Level (Can)))
      then
         if Name = "CONSTRAINT_ERROR" then
            return Constraint_Error_Tag (Ctx);
         elsif Name = "NUMERIC_ERROR" then
            -- ACVC 1.11 (cb2004a): keep NUMERIC_ERROR distinct on explicit raise/catch
            -- while arithmetic checks raise CONSTRAINT_ERROR per AI-00387.
            Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_numeric_error");
            return "@__ada_exc_numeric_error";
         elsif Name = "PROGRAM_ERROR" then
            return Program_Error_Tag (Ctx);
         elsif Name = "STORAGE_ERROR" then
            return Storage_Error_Tag (Ctx);
         elsif Name = "TASKING_ERROR" then
            Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_tasking_error");
            return "@__ada_exc_tasking_error";
         end if;
      elsif Pkg = "IO_EXCEPTIONS" and then Is_Library_Level (Can) then
         declare
            S : constant String := "@__ada_exc_" & Lower_Name (Name);
         begin
            Ensure_Std_Exc_Extern (Ctx, S);
            return S;
         end;
      elsif Pkg = "CALENDAR" and then Name = "TIME_ERROR" and then Is_Library_Level (Can) then
         Ensure_Std_Exc_Extern (Ctx, "@__ada_exc_time_error");
         return "@__ada_exc_time_error";
      end if;

      if Ctx.Exc_Tags.Contains (Key) then
         return To_String (Ctx.Exc_Tags.Element (Key));
      end if;

      -- Library-level user exception or ancestor exception accessed from a subunit: reference via extern symbol
      if Is_Library_Level (Can) or else Ctx.In_Subunit then
         declare
            Sym : constant String := "@__ada_exc__" & Qualified_Ada_Name (Can);
         begin
            Ensure_Std_Exc_Extern (Ctx, Sym);
            Ctx.Exc_Tags.Include (Key, To_Unbounded_String (Sym));
            return Sym;
         end;
      end if;

      -- Generic-local exception: tag with Gen_Inst_Tag and keep in Gen_Local_Exc_Tags so
      -- distinct instantiations never share exception identity!
      if Length (Ctx.Gen_Inst_Tag) > 0 and then Ctx.In_Gen_Body then
         declare
            QName : constant String := Qualified_Ada_Name (Can);
            Sym   : constant String := "@__ada_exc_" & QName & "_" & Tree_Key_Str (Can) & To_String (Ctx.Gen_Inst_Tag);
         begin
            Add_Data_Decl (Ctx.E, Sym,
              Sym & " = data align 1 { """ & Escape_Pir_String (Name) & "\0"" }");
            Ctx.Gen_Local_Exc_Tags.Include (Key, To_Unbounded_String (Sym));
            return Sym;
         end;
      end if;

      declare
         QName : constant String := Qualified_Ada_Name (Can);
         Sym   : constant String := "@__ada_exc_" & QName & "_" & Tree_Key_Str (Can);
      begin
         Add_Data_Decl (Ctx.E, Sym,
           Sym & " = data align 1 { """ & Escape_Pir_String (Name) & "\0"" }");
         Ctx.Exc_Tags.Include (Key, To_Unbounded_String (Sym));
         return Sym;
      end;
   end Get_Exc_Tag;

   -- Build PIR struct type for a record (including discriminants and optional variant part)
   function Build_Record_Type (Ctx : in out Context; Rec_Spec : TREE) return Type_Info is
      Base_Rec : constant TREE := Subst_Base_Type (Ctx, Rec_Spec);
      Key      : constant Unsigned_32 := Tree_Key (Base_Rec);
      TName    : constant String :=
        "@ada_rec_" & Tree_Key_Str (Base_Rec) &
        (if not Ctx.Generic_Subst.Is_Empty then To_String (Ctx.Gen_Inst_Tag) else "");
      Info     : Type_Info;
      Fields_S : Unbounded_String;
      FIdx     : Natural := 0;
      Offset   : Natural := 0;
      Max_Al   : Natural := 1;

      procedure Append_Field (Comp_Id : TREE; FT : Type_Info) is
         FE      : Field_Entry;
         Is_DynA : constant Boolean := FT.Kind = TK_Constrained_Array and then FT.Is_Dynamic;
         F_Pir   : constant String := (if Is_DynA then "@__ada_arr_desc" else To_String (FT.Pir_Type));
         F_Sz    : constant Natural := (if Is_DynA then Arr_Desc_Size else FT.Byte_Size);
         F_Al    : constant Natural := (if Is_DynA then 8 else FT.Align);
         Off_Al  : constant Natural := Align_Up (Offset, F_Al);
      begin
         if FIdx > 0 then
            Append (Fields_S, ", ");
         end if;
         Append (Fields_S, F_Pir);
         Offset := Off_Al + F_Sz;
         if F_Al > Max_Al then
            Max_Al := F_Al;
         end if;
         FE.Field_Idx   := FIdx;
         FE.In_Union    := False;
         FE.Byte_Offset := Off_Al;
         FE.Field_Size  := F_Sz;
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

         -- Variant part -> PIR union of variant structs (supports arbitrary nesting)
         declare
            procedure Build_Variant_Part
              (VP         : TREE;
               Union_Sym  : String;
               Union_Pos  : Natural;
               Out_Max_Sz : out Natural;
               Out_Max_Al : out Natural;
               Out_Keys   : out U32_Vecs.Vector)
            is
               VS       : constant TREE := D (AS_VARIANT_S, VP);
               VList    : SEQ_TYPE := (if not Is_Void_Tree (VS) then LIST (VS) else MAKE);
               VNode    : TREE;
               U_Fields : Unbounded_String;
               U_Max_Sz : Natural := 1;
               U_Max_Al : Natural := 1;
               VCount   : Natural := 0;
            begin
               Out_Keys := U32_Vecs.Empty_Vector;
               while not IS_EMPTY (VList) loop
                  POP (VList, VNode);
                  if KIND (VNode) = DN_VARIANT then
                     declare
                        VCL    : constant TREE := D (AS_COMP_LIST, VNode);
                        VSym   : constant String := Union_Sym & "_v" & Img (VCount);
                        VF_Str : Unbounded_String;
                        VF_Idx : Natural := 0;
                        V_Off  : Natural := 0;
                        V_Al   : Natural := 1;
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
                                                CT      : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, VCID));
                                                FT      : constant Type_Info := Get_Type_Info (Ctx, CT);
                                                Is_DynA : constant Boolean := FT.Kind = TK_Constrained_Array and then FT.Is_Dynamic;
                                                F_Pir   : constant String := (if Is_DynA then "@__ada_arr_desc" else To_String (FT.Pir_Type));
                                                F_Sz    : constant Natural := (if Is_DynA then Arr_Desc_Size else FT.Byte_Size);
                                                F_Al    : constant Natural := (if Is_DynA then 8 else FT.Align);
                                                V_Off_A : constant Natural := Align_Up (V_Off, F_Al);
                                                FE      : Field_Entry;
                                                VCK     : constant Unsigned_32 := Tree_Key (VCID);
                                             begin
                                                if VF_Idx > 0 then
                                                   Append (VF_Str, ", ");
                                                end if;
                                                Append (VF_Str, F_Pir);
                                                V_Off := V_Off_A + F_Sz;
                                                if F_Al > V_Al then V_Al := F_Al; end if;
                                                FE.Field_Idx   := Union_Pos;
                                                FE.In_Union    := True;
                                                FE.Union_Idx   := Union_Pos;
                                                FE.Var_Idx     := VCount;
                                                FE.Var_Type    := To_Unbounded_String (VSym);
                                                FE.Var_Field   := VF_Idx;
                                                FE.Byte_Offset := V_Off_A;
                                                FE.Field_Size  := F_Sz;
                                                Ctx.Fields.Include (VCK, FE);
                                                Out_Keys.Append (VCK);
                                                VF_Idx := VF_Idx + 1;
                                             end;
                                          end loop;
                                       end;
                                    end if;
                                 end loop;
                              end if;
                           end;
                           declare
                              Sub_VP : constant TREE := D (AS_VARIANT_PART, VCL);
                           begin
                              if not Is_Void_Tree (Sub_VP) and then KIND (Sub_VP) = DN_VARIANT_PART then
                                 declare
                                    Sub_USym : constant String := VSym & "_u" & Img (VF_Idx);
                                    Sub_Sz   : Natural := 0;
                                    Sub_Al   : Natural := 1;
                                    Sub_Keys : U32_Vecs.Vector;
                                    Sub_OffA : Natural;
                                 begin
                                    Build_Variant_Part (Sub_VP, Sub_USym, Union_Pos, Sub_Sz, Sub_Al, Sub_Keys);
                                    if Sub_Sz > 0 then
                                       Sub_OffA := Align_Up (V_Off, Sub_Al);
                                       if VF_Idx > 0 then
                                          Append (VF_Str, ", ");
                                       end if;
                                       Append (VF_Str, Sub_USym);
                                       V_Off := Sub_OffA + Sub_Sz;
                                       if Sub_Al > V_Al then V_Al := Sub_Al; end if;
                                       VF_Idx := VF_Idx + 1;
                                       for SK of Sub_Keys loop
                                          declare
                                             FE : Field_Entry := Ctx.Fields.Element (SK);
                                          begin
                                             FE.Byte_Offset := FE.Byte_Offset + Sub_OffA;
                                             Ctx.Fields.Include (SK, FE);
                                             Out_Keys.Append (SK);
                                          end;
                                       end loop;
                                    end if;
                                 end;
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
                  Out_Max_Sz := U_Max_Sz;
                  Out_Max_Al := U_Max_Al;
               else
                  Out_Max_Sz := 0;
                  Out_Max_Al := 1;
               end if;
            end Build_Variant_Part;

            VP : constant TREE := D (AS_VARIANT_PART, CL);
         begin
            if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART then
               declare
                  Union_Sym : constant String := TName & "_u" & Img (FIdx);
                  U_Max_Sz  : Natural := 0;
                  U_Max_Al  : Natural := 1;
                  U_Keys    : U32_Vecs.Vector;
                  U_Off_A   : Natural;
               begin
                  Build_Variant_Part (VP, Union_Sym, FIdx, U_Max_Sz, U_Max_Al, U_Keys);
                  if U_Max_Sz > 0 then
                     U_Off_A := Align_Up (Offset, U_Max_Al);
                     for UK of U_Keys loop
                        declare
                           FE : Field_Entry := Ctx.Fields.Element (UK);
                        begin
                           FE.Byte_Offset := FE.Byte_Offset + U_Off_A;
                           Ctx.Fields.Include (UK, FE);
                        end;
                     end loop;
                     if FIdx > 0 then
                        Append (Fields_S, ", ");
                     end if;
                     Append (Fields_S, Union_Sym);
                     Offset := U_Off_A + U_Max_Sz;
                     if U_Max_Al > Max_Al then Max_Al := U_Max_Al; end if;
                     FIdx := FIdx + 1;
                  end if;
               end;
            end if;
         end;
      end Walk_Comp_List_Fixed;
   begin
      if Ctx.Generic_Subst.Is_Empty and then Ctx.Types.Contains (Key) then
         return Ctx.Types.Element (Key);
      end if;
      -- Cycle guard for recursive access-to-record definitions (separate from Ctx.Types)
      if Ctx.In_Progress_Recs.Contains (Key) then
         Info.Kind := TK_Record;
         Info.Pir_Type := To_Unbounded_String (TName);
         Info.Byte_Size := 8;
         Info.Align := 8;
         return Info;
      end if;
      Info.Kind := TK_Record;
      Info.Pir_Type := To_Unbounded_String (TName);
      Info.Byte_Size := 8;
      Info.Align := 8;

      if Is_Void_Tree (Base_Rec) or else KIND (Base_Rec) /= DN_RECORD then
         Add_Type_Decl (Ctx.E, "type " & TName & " = { i64 }");
         Info.Byte_Size := 8;
         Info.Align := 8;
         if Ctx.Generic_Subst.Is_Empty then
            Ctx.Types.Include (Key, Info);
         end if;
         return Info;
      end if;

      Ctx.In_Progress_Recs.Include (Key);

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

      -- LRM 13.4: apply record representation clause (`for R use record [at mod N;] ... end record;`)
      declare
         RR : constant TREE := D (SM_REPRESENTATION, Base_Rec);
      begin
         if not Is_Void_Tree (RR) and then KIND (RR) = DN_RECORD_REP then
            declare
               AC      : constant TREE := D (AS_ALIGNMENT_CLAUSE, RR);
               CRS     : constant TREE := D (AS_COMP_REP_S, RR);
               Rep_Al  : Natural := 1;
               Max_End : Natural := 0;
               Placed  : U32_Vecs.Vector;

               function Is_Placed (K : Unsigned_32) return Boolean is
               begin
                  for P of Placed loop
                     if P = K then return True; end if;
                  end loop;
                  return False;
               end Is_Placed;

               procedure Place_Unmentioned_List (DS : TREE) is
                  DL : SEQ_TYPE;
                  DD : TREE;
               begin
                  if Is_Void_Tree (DS) then return; end if;
                  DL := LIST (DS);
                  while not IS_EMPTY (DL) loop
                     POP (DL, DD);
                     if KIND (DD) in DN_DSCRMT_DECL | DN_VARIABLE_DECL then
                        declare
                           NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                           CID : TREE;
                        begin
                           while not IS_EMPTY (NS) loop
                              POP (NS, CID);
                              declare
                                 CK : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                              begin
                                 if Ctx.Fields.Contains (CK) and then not Is_Placed (CK) then
                                    declare
                                       FE  : Field_Entry := Ctx.Fields.Element (CK);
                                       CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                                       FT  : constant Type_Info := Get_Type_Info (Ctx, CT);
                                       Al  : constant Natural := Natural'Max (1, FT.Align);
                                       Off : constant Natural := Align_Up (Max_End, Al);
                                    begin
                                       FE.Has_Explicit_Offset := True;
                                       FE.Byte_Offset := Off;
                                       FE.Bit_Offset  := 0;
                                       FE.Bit_Size    := 0;
                                       Ctx.Fields.Include (CK, FE);
                                       Placed.Append (CK);
                                       Max_End := Off + Natural'Max (1, FE.Field_Size);
                                       if Al > Rep_Al then Rep_Al := Al; end if;
                                    end;
                                 end if;
                              end;
                           end loop;
                        end;
                     end if;
                  end loop;
               end Place_Unmentioned_List;

               procedure Place_Unmentioned_CL (CL : TREE) is
                  VP : TREE;
               begin
                  if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return; end if;
                  Place_Unmentioned_List (D (AS_DECL_S, CL));
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
                              Place_Unmentioned_CL (D (AS_COMP_LIST, VN));
                           end if;
                        end loop;
                     end;
                  end if;
               end Place_Unmentioned_CL;
            begin
               if not Is_Void_Tree (AC) and then not Is_Void_Tree (D (AS_EXP, AC)) then
                  declare
                     AV : constant TREE := Safe_SM_Value (D (AS_EXP, AC));
                  begin
                     if Has_Static_Val (AV) then
                        declare
                           AI : constant Integer_64 := Eval_Int64 (AV);
                        begin
                           if AI >= 1 and then AI <= 64 then
                              Rep_Al := Natural (AI);
                           end if;
                        end;
                     end if;
                  end;
               end if;
               if not Is_Void_Tree (CRS) then
                  declare
                     CRL : SEQ_TYPE := LIST (CRS);
                     CR  : TREE;
                  begin
                     while not IS_EMPTY (CRL) loop
                        POP (CRL, CR);
                        if KIND (CR) = DN_COMP_REP then
                           declare
                              CID   : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, CR)));
                              CK    : constant Unsigned_32 := Tree_Key (CID);
                              Pos_E : constant TREE := D (AS_EXP, CR);
                              Rng   : constant TREE := D (AS_RANGE, CR);
                              Pos_B : Integer_64 := 0;
                              Lo_B  : Integer_64 := 0;
                              Hi_B  : Integer_64 := 7;
                           begin
                              if not Is_Void_Tree (Pos_E) and then Has_Static_Val (Safe_SM_Value (Pos_E)) then
                                 Pos_B := Eval_Int64 (Safe_SM_Value (Pos_E));
                              end if;
                              if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                                 if Has_Static_Val (Safe_SM_Value (D (AS_EXP1, Rng))) then
                                    Lo_B := Eval_Int64 (Safe_SM_Value (D (AS_EXP1, Rng)));
                                 end if;
                                 if Has_Static_Val (Safe_SM_Value (D (AS_EXP2, Rng))) then
                                    Hi_B := Eval_Int64 (Safe_SM_Value (D (AS_EXP2, Rng)));
                                 end if;
                              end if;
                              if Pos_B >= 0 and then Lo_B >= 0 and then Hi_B >= Lo_B and then Ctx.Fields.Contains (CK) then
                                 declare
                                    Start_Bit : constant Integer_64 := Pos_B * 8 + Lo_B;
                                    End_Bit   : constant Integer_64 := Pos_B * 8 + Hi_B;
                                    End_Byte  : constant Natural := Natural (End_Bit / 8) + 1;
                                    FE        : Field_Entry := Ctx.Fields.Element (CK);
                                 begin
                                    FE.Has_Explicit_Offset := True;
                                    FE.Byte_Offset := Natural (Start_Bit / 8);
                                    FE.Bit_Offset  := Natural (Start_Bit mod 8);
                                    -- P3-6.3: Only treat scalar fields as scalar bitfields; composite fields at byte boundary use byte offset
                                    declare
                                       CT_R : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                                       FT_R : constant Type_Info := Get_Type_Info (Ctx, CT_R);
                                       Comp_End : Natural := End_Byte;
                                    begin
                                       if FT_R.Kind = TK_Scalar then
                                          FE.Bit_Size := Natural (End_Bit - Start_Bit + 1);
                                       else
                                          FE.Bit_Size := 0;
                                          if FE.Byte_Offset + FT_R.Byte_Size > Comp_End then
                                             Comp_End := FE.Byte_Offset + FT_R.Byte_Size;
                                          end if;
                                       end if;
                                       Ctx.Fields.Include (CK, FE);
                                       Placed.Append (CK);
                                       if Comp_End > Max_End then
                                          Max_End := Comp_End;
                                       end if;
                                    end;
                                 end;
                              end if;
                           end;
                        end if;
                     end loop;
                  end;
               end if;
               -- Place any unmentioned components after explicitly placed ones
               Place_Unmentioned_List (D (SM_DISCRIMINANT_S, Base_Rec));
               Place_Unmentioned_CL (D (SM_COMP_LIST, Base_Rec));
               if Max_End = 0 then
                  Max_End := 1;
               end if;
               -- P3-6.5: Honor explicit `for R'Size use N;` if larger than placed fields
               declare
                  Sz_A : constant TREE := D (SM_SIZE, Base_Rec);
               begin
                  if Has_Static_Val (Sz_A) then
                     declare
                        SB : constant Integer_64 := Eval_Int64 (Sz_A);
                     begin
                        if SB > 0 and then SB < 8_000_000 then
                           declare
                              Min_B : constant Natural := Natural ((SB + 7) / 8);
                           begin
                              if Min_B > Max_End then
                                 Max_End := Min_B;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;
               -- Round Rep_Al up to a valid power-of-two alignment for PIR `type @T = { ... }, align N`
               declare
                  P2_Al : Natural := 1;
               begin
                  while P2_Al < Rep_Al and then P2_Al < 64 loop
                     P2_Al := P2_Al * 2;
                  end loop;
                  Rep_Al := P2_Al;
               end;
               Offset := Align_Up (Max_End, Rep_Al);
               Max_Al := Rep_Al;
               Add_Type_Decl (Ctx.E,
                 "type " & TName & " = { [" & Img (Offset) & " x i8] }" &
                 (if Max_Al > 1 then ", align " & Img (Max_Al) else ""));
               Info.Byte_Size := Offset;
               Info.Align := Max_Al;
               Ctx.In_Progress_Recs.Exclude (Key);
               if Ctx.Generic_Subst.Is_Empty then
                  Ctx.Types.Include (Key, Info);
               end if;
               return Info;
            end;
         end if;
      end;

      if FIdx = 0 then
         -- Empty record (e.g. null;) -> single i8 dummy byte
         Fields_S := To_Unbounded_String ("i8");
         Offset := 1;
         Max_Al := 1;
      else
         Offset := Align_Up (Offset, Max_Al);
      end if;

      -- Honor explicit `for R'Size use N;` on non-rep-clause records if larger than natural size
      declare
         Sz_A : constant TREE := D (SM_SIZE, Base_Rec);
      begin
         if Has_Static_Val (Sz_A) then
            declare
               SB : constant Integer_64 := Eval_Int64 (Sz_A);
            begin
               if SB > 0 and then SB < 8_000_000 then
                  declare
                     Min_B : constant Natural := Align_Up (Natural ((SB + 7) / 8), Max_Al);
                  begin
                     if Min_B > Offset then
                        Append (Fields_S, ", [" & Img (Min_B - Offset) & " x i8]");
                        Offset := Min_B;
                     end if;
                  end;
               end if;
            end;
         end if;
      end;

      Add_Type_Decl (Ctx.E, "type " & TName & " = { " & To_String (Fields_S) & " }");
      Info.Byte_Size := Offset;
      Info.Align := Max_Al;
      Ctx.In_Progress_Recs.Exclude (Key);
      if Ctx.Generic_Subst.Is_Empty then
         Ctx.Types.Include (Key, Info);
      end if;
      return Info;
   end Build_Record_Type;

   function Get_Type_Info (Ctx : in out Context; Raw_Type : TREE) return Type_Info is
      R   : constant TREE := Subst_Resolve_Type (Ctx, Raw_Type);
      Key : constant Unsigned_32 := Tree_Key (R);
      Info : Type_Info;
   begin
      if Is_Void_Tree (R) then
         Info.Kind := TK_Void;
         Info.Pir_Type := To_Unbounded_String ("void");
         return Info;
      end if;

      if Ctx.Generic_Subst.Is_Empty and then Ctx.Types.Contains (Key) then
         return Ctx.Types.Element (Key);
      end if;

      if Is_Task_Type (R) then
         Info.Kind      := TK_Scalar;
         Info.Pir_Type  := To_Unbounded_String ("ptr");
         Info.Byte_Size := 8;
         Info.Align     := 8;
         if Ctx.Generic_Subst.Is_Empty then Ctx.Types.Include (Key, Info); end if;
         return Info;

      elsif Is_Scalar_Type (R) then
         Info.Kind := TK_Scalar;
         Info.Pir_Type := To_Unbounded_String (Pir_Scalar_Type (R));
         declare
            Bits : constant Natural := Scalar_Bits (R);
         begin
            Info.Byte_Size := Bits / 8;
            if Info.Byte_Size = 0 then Info.Byte_Size := 1; end if;
            Info.Align := Info.Byte_Size;
         end;
         if Ctx.Generic_Subst.Is_Empty then Ctx.Types.Include (Key, Info); end if;
         return Info;

      elsif Is_Record_Type (R) or else (KIND (R) = DN_CONSTRAINED_RECORD and then Is_Record_Type (Subst_Base_Type (Ctx, R))) then
         Info := Build_Record_Type (Ctx, R);
         if Ctx.Generic_Subst.Is_Empty then
            Ctx.Types.Include (Key, Info);
         end if;
         return Info;

      elsif Is_Constrained_Array (R) then
         declare
            Base   : constant TREE := Subst_Base_Type (Ctx, R);
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
            while not IS_EMPTY (List_S) and then Dim < Max_Array_Dims loop
               POP (List_S, Idx_T);
               Idx_T := Subst_Resolve_Type (Ctx, Idx_T);
               Dim := Dim + 1;
               declare
                  St : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Get_Discrete_Bounds (Idx_T, St, Lo, Hi);
                  Info.Dims (Dim) := (Is_Static => St, First => Lo, Last => Hi, Idx_Type => Idx_T);
                  if St then
                     if Hi >= Lo then
                        if Hi = Integer_64'Last and then Lo = Integer_64'First then
                           All_St := False;
                        else
                           declare
                              U_Hi : constant Unsigned_64 := Unsigned_64'Mod (Hi);
                              U_Lo : constant Unsigned_64 := Unsigned_64'Mod (Lo);
                              Span : constant Unsigned_64 := (U_Hi - U_Lo) + 1;
                           begin
                              if Span > 16_777_216 or else Unsigned_64 (Total) * Span > 16_777_216 then
                                 All_St := False;
                              else
                                 Total := Total * Integer_64 (Span);
                              end if;
                           end;
                        end if;
                     else
                        Total := 0;
                     end if;
                  else
                     All_St := False;
                  end if;
               end;
            end loop;
            Info.NDims := Dim;
            declare
               Is_Dyn_Elem : constant Boolean := EI.Kind = TK_Constrained_Array and then EI.Is_Dynamic;
               E_Pir       : constant String := (if Is_Dyn_Elem then "@__ada_arr_desc" else To_String (EI.Pir_Type));
               E_Sz        : constant Natural := (if Is_Dyn_Elem then Arr_Desc_Size else EI.Byte_Size);
               E_Al        : constant Natural := (if Is_Dyn_Elem then 8 else EI.Align);
            begin
               Info.Align := E_Al;
               if All_St and then Total <= 16_777_216 then
                  Info.Is_Dynamic := False;
                  declare
                     N_Elems : constant Long_Long_Integer :=
                       Long_Long_Integer'Max (1, Long_Long_Integer (Total));
                     Elem_Stride : constant Long_Long_Integer :=
                       Long_Long_Integer (Align_Up (E_Sz, E_Al));
                     Tot_Bytes : constant Long_Long_Integer := N_Elems * Elem_Stride;
                     TName   : constant String :=
                       "@ada_arr_" & Tree_Key_Str (R) &
                       (if not Ctx.Generic_Subst.Is_Empty then To_String (Ctx.Gen_Inst_Tag) else "");
                  begin
                     if Tot_Bytes <= 67_108_864 then
                        Add_Type_Decl (Ctx.E,
                          "type " & TName & " = [" & Img (N_Elems) & " x " & E_Pir & "]");
                        Info.Pir_Type := To_Unbounded_String (TName);
                        Info.Byte_Size := Natural (Tot_Bytes);
                     else
                        Info.Is_Dynamic := True;
                        Info.Pir_Type := To_Unbounded_String ("ptr");
                        Info.Byte_Size := 8;
                        Info.Align := 8;
                     end if;
                  end;
               else
                  Info.Is_Dynamic := True;
                  Info.Pir_Type := To_Unbounded_String ("ptr");
                  Info.Byte_Size := 8;
                  Info.Align := 8;
               end if;
            end;
            if Ctx.Generic_Subst.Is_Empty then
               Ctx.Types.Include (Key, Info);
            end if;
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
            while not IS_EMPTY (IL) and then Dim < Max_Array_Dims loop
               POP (IL, Idx_N);
               Dim := Dim + 1;
               Info.Dims (Dim).Idx_Type := Subst_Resolve_Type (Ctx, D (SM_TYPE_SPEC, Idx_N));
            end loop;
            Info.NDims := Dim;
            Info.Pir_Type := To_Unbounded_String ("ptr");
            Info.Byte_Size := 8;
            Info.Align := 8;
            if Ctx.Generic_Subst.Is_Empty then
               Ctx.Types.Include (Key, Info);
            end if;
            return Info;
         end;

      else
         Info.Kind := TK_Scalar;
         Info.Pir_Type := To_Unbounded_String ("i64");
         Info.Byte_Size := 8;
         Info.Align := 8;
         if Ctx.Generic_Subst.Is_Empty then
            Ctx.Types.Include (Key, Info);
         end if;
         return Info;
      end if;
   end Get_Type_Info;

   function Is_C_Interfaced (Subp_Id : TREE) return Boolean is
      Can   : constant TREE := Canonical_Def (Subp_Id);
      Iface : TREE;
   begin
      if Is_Void_Tree (Can) or else KIND (Can) not in CLASS_SUBPROG_NAME then
         return False;
      end if;
      Iface := D (SM_INTERFACE, Can);
      if Is_Void_Tree (Iface) then
         return False;
      end if;
      return Sym_Name (Iface) = "C";
   end Is_C_Interfaced;

   function Mangle_Subp_Name
     (Ctx        : in out Context;
      Subp_Id    : TREE;
      Spec_Node  : TREE;
      Is_Subunit : Boolean := False) return String
   is
      Can  : constant TREE := Canonical_Def (Subp_Id);
      Base : constant String := Qualified_Ada_Name (Can);
      Sig  : Unbounded_String := To_Unbounded_String ("@ada__" & Base);

      function Ada_Type_Tag (T : TREE) return String is
         BT  : constant TREE := Subst_Base_Type (Ctx, T);
         TI  : constant Type_Info := Get_Type_Info (Ctx, T);
         Src : TREE := CONST_VOID;
      begin
         if not Is_Void_Tree (BT) and then KIND (BT) in CLASS_NON_TASK | DN_TASK_SPEC then
            Src := D (XD_SOURCE_NAME, BT);
         end if;
         if not Is_Void_Tree (Src) then
            declare
               QN : constant String := Qualified_Ada_Name (Src);
            begin
               if QN'Length > 0 then
                  return QN;
               end if;
            end;
         end if;
         if TI.Kind = TK_Scalar then
            return To_String (TI.Pir_Type);
         elsif TI.Kind = TK_Record then
            return "rec";
         elsif TI.Kind = TK_Unconstrained_Array then
            return "uarr" & Img (TI.NDims);
         else
            return "arr";
         end if;
      end Ada_Type_Tag;
   begin
      if Is_C_Interfaced (Can) then
         return "@" & Sanitize_Ident (Sym_Name (Can));
      end if;
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
                        begin
                           Append (Sig, "__p_" & Ada_Type_Tag (PT));
                        end;
                     end loop;
                  end;
               end loop;
            end if;
         end;
         if KIND (Spec_Node) = DN_FUNCTION_SPEC then
            declare
               RDef : constant TREE := Get_Name_Defn (D (AS_NAME, Spec_Node));
               RT   : constant TREE := (if not Is_Void_Tree (RDef) then Subst_Tree (Ctx, D (SM_TYPE_SPEC, RDef)) else CONST_VOID);
            begin
               Append (Sig, "__ret_" & Ada_Type_Tag (RT));
            end;
         end if;
      end if;
      -- Disambiguate any non-library-level subprogram (except separate subunits, which link across .o files!)
      if not Is_Library_Level (Can) and then not Is_Subunit then
         Append (Sig, "_" & Tree_Key_Str (Can));
         if Length (Ctx.Gen_Inst_Tag) > 0 then
            Append (Sig, To_String (Ctx.Gen_Inst_Tag));
         end if;
      end if;
      declare
         Full : constant String := To_String (Sig);
      begin
         -- Cap very long symbol names (> 96 chars) deterministically with 64-bit FNV-1a hash
         -- so 200-char ACVC identifiers never truncate/collide in PIR lexer or assembler!
         if Full'Length > 96 then
            declare
               H : Unsigned_64 := 16#CBF2_9CE4_8422_2325#;
               Hex : constant String := "0123456789abcdef";
               HStr : String (1 .. 16);
               Tmp : Unsigned_64;
            begin
               for C of Full loop
                  H := (H xor Character'Pos (C)) * 16#0000_0100_0000_01B3#;
               end loop;
               Tmp := H;
               for I in reverse 1 .. 16 loop
                  HStr (I) := Hex (Natural (Tmp and 15) + 1);
                  Tmp := Shift_Right (Tmp, 4);
               end loop;
               declare
                  GI_Sfx : constant String :=
                    (if not Is_Library_Level (Can) and then not Is_Subunit then To_String (Ctx.Gen_Inst_Tag) else "");
               begin
                  return Full (Full'First .. Full'First + 47) & "_h" & HStr & GI_Sfx;
               end;
            end;
         end if;
         return Full;
      end;
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
      -- Never resurrect dead code after a terminator (would create a zero-predecessor parameterized block!)
      if Is_Terminated (Ctx.E) then
         if Ret_Type'Length > 0 and then Ret_Type /= "void" then
            if Ret_Type = "ptr" then
               Res_Reg := To_Unbounded_String ("null");
            elsif Ret_Type = "f32" or else Ret_Type = "f64" then
               Res_Reg := To_Unbounded_String ("0.0");
            else
               Res_Reg := To_Unbounded_String ("0");
            end if;
         end if;
         return;
      end if;
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

   function Record_Has_Default_Discs (Ctx : Context; Raw_T : TREE) return Boolean is
      BT : constant TREE := Subst_Base_Type (Ctx, Raw_T);
      DS : TREE;
   begin
      if Is_Void_Tree (BT) then
         return False;
      end if;
      if KIND (BT) = DN_RECORD or else KIND (BT) = DN_PRIVATE or else KIND (BT) = DN_L_PRIVATE then
         DS := D (SM_DISCRIMINANT_S, BT);
         if not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS)) then
            declare
               DL : SEQ_TYPE := LIST (DS);
               DD : TREE;
            begin
               POP (DL, DD);
               if KIND (DD) = DN_DSCRMT_DECL then
                  if not Is_Void_Tree (D (AS_EXP, DD)) then
                     return True;
                  end if;
                  declare
                     NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                     DID : TREE;
                  begin
                     if not IS_EMPTY (NS) then
                        POP (NS, DID);
                        return not Is_Void_Tree (D (SM_INIT_EXP, DID));
                     end if;
                  end;
               end if;
            end;
         end if;
      end if;
      return False;
   end Record_Has_Default_Discs;

   function Needs_Constrained_Param (Ctx : Context; Raw_T : TREE; Mode : NODE_NAME) return Boolean is
      TS : constant TREE := Subst_Resolve_Type (Ctx, Raw_T);
   begin
      if (Mode /= DN_OUT and then Mode /= DN_IN_OUT) or else Is_Void_Tree (TS) then
         return False;
      end if;
      if KIND (TS) in DN_RECORD | DN_PRIVATE | DN_L_PRIVATE then
         return Record_Has_Default_Discs (Ctx, TS);
      end if;
      return False;
   end Needs_Constrained_Param;

   function Explicit_Field_Align (FE : Field_Entry; Nat_Align : Natural) return Natural is
   begin
      if FE.Has_Explicit_Offset then
         return 1;
      end if;
      return Natural'Max (1, Nat_Align);
   end Explicit_Field_Align;

   function Build_Param_Sig
     (Ctx            : in out Context;
      Spec_Node      : TREE;
      Needs_SL       : Boolean;
      Ret_Comp       : Boolean;
      Ret_Type       : TREE;
      With_Names     : Boolean;
      Is_C_Interface : Boolean := False) return String
   is
      Buf   : Unbounded_String;
      First : Boolean := True;

      procedure Add_P (Name : String; Ty : String) is
      begin
         if not First then
            Append (Buf, ", ");
         end if;
         First := False;
         Append (Buf, Name & ": " & Ty);
      end Add_P;
   begin
      pragma Unreferenced (With_Names);
      if Needs_SL and then not Is_C_Interface then
         Add_P ("%__sl", "ptr");
      end if;
      if Ret_Comp and then not Is_C_Interface then
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
                           if TI.Kind = TK_Unconstrained_Array
                             or else (TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic)
                           then
                              Add_P (PN & "_ptr", "ptr");
                              if not Is_C_Interface then
                                 for D_Idx in 1 .. TI.NDims loop
                                    Add_P (PN & "_f" & Img (D_Idx), "i64");
                                    Add_P (PN & "_l" & Img (D_Idx), "i64");
                                 end loop;
                              end if;
                           elsif TI.Kind = TK_Record or else TI.Kind = TK_Constrained_Array then
                              Add_P (PN, "ptr");
                              if not Is_C_Interface and then Needs_Constrained_Param (Ctx, PT, Mode) then
                                 Add_P (PN & "_con", "i8");
                              end if;
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
      Is_C : constant Boolean := Info.Is_C_Interface or else Is_C_Interfaced (Subp_Id);
      Params : constant String :=
        Build_Param_Sig (Ctx, Info.Spec_Node, (if Is_C then False else Info.Needs_SL), Info.Ret_Composite, Info.Ret_Type, True, Is_C_Interface => Is_C);
      CC  : constant String := (if Is_C then """c""" else """internal""");
   begin
      if Has_Symbol (Ctx.E, Sym) then
         return;
      end if;
      if not Info.Is_Function then
         Ret := To_Unbounded_String ("void");
      elsif Info.Ret_Composite then
         if Is_C then
            declare
               RTI : constant Type_Info := Get_Type_Info (Ctx, Info.Ret_Type);
            begin
               Ret := To_Unbounded_String (if RTI.Kind = TK_Scalar then To_String (RTI.Pir_Type) else "ptr");
            end;
         else
            Ret := To_Unbounded_String ("void");
         end if;
      else
         Ret := Get_Type_Info (Ctx, Info.Ret_Type).Pir_Type;
      end if;
      Add_Extern_Func (Ctx.E, Sym,
        "extern func " & Sym & "(" & Params & ") -> " & To_String (Ret) & " cc " & CC);
   end Ensure_Extern_Subp;

   -- Centralized static-link chain walker: returns the frame pointer & Frame_Type of Target_Owner
   procedure Walk_Static_Link
     (Ctx          : in out Context;
      Target_Owner : Unsigned_32;
      Hoist_Entry  : Boolean;
      Found        : out Boolean;
      Frame_Ptr    : out Unbounded_String;
      Frame_Type   : out Unbounded_String)
   is
      Cur_K : Unsigned_32 := Ctx.Cur_Subp_Key;
      SL    : Unbounded_String := Ctx.Cur_SL_Reg;
   begin
      Found := False;
      Frame_Ptr := Null_Unbounded_String;
      Frame_Type := Null_Unbounded_String;
      if Length (SL) = 0 or else Target_Owner = 0 then
         return;
      end if;
      for Hop in 1 .. 32 loop
         exit when not Ctx.Subps.Contains (Cur_K);
         declare
            SI       : constant Subp_Info := Ctx.Subps.Element (Cur_K);
            Parent_K : constant Unsigned_32 := SI.Parent_Subp;
         begin
            exit when Parent_K = 0 or else not Ctx.Subps.Contains (Parent_K);
            declare
               PI : constant Subp_Info := Ctx.Subps.Element (Parent_K);
            begin
               exit when not PI.Has_Frame or else Length (PI.Frame_Type) = 0;
               if Parent_K = Target_Owner then
                  Found := True;
                  Frame_Ptr := SL;
                  Frame_Type := PI.Frame_Type;
                  return;
               else
                  declare
                     FP  : constant String := Next_Reg (Ctx.E, "slp");
                     NSL : constant String := Next_Reg (Ctx.E, "sl");
                     I1  : constant String := FP & " = field_ptr " & To_String (PI.Frame_Type) & ", " & To_String (SL) & ", 0";
                     I2  : constant String := NSL & " = load.ptr " & FP & ", align 8";
                  begin
                     if Hoist_Entry then
                        Emit_Entry_Alloca (Ctx.E, I1);
                        Emit_Entry_Alloca (Ctx.E, I2);
                     else
                        Emit_Inst (Ctx.E, I1);
                        Emit_Inst (Ctx.E, I2);
                     end if;
                     SL := To_Unbounded_String (NSL);
                     Cur_K := Parent_K;
                  end;
               end if;
            end;
         end;
      end loop;
   end Walk_Static_Link;

   function Emit_Var_Addr (Ctx : in out Context; V : Var_Info) return String is
   begin
      if V.Kind = VK_Global then
         declare
            R : constant String := Next_Reg (Ctx.E, "ga");
         begin
            Emit_Inst (Ctx.E, R & " = addr " & To_String (V.Pir_Name));
            return R;
         end;
      elsif V.Kind = VK_Global_Ptr then
         declare
            GA : constant String := Next_Reg (Ctx.E, "gpa");
            LP : constant String := Next_Reg (Ctx.E, "gpl");
         begin
            Emit_Inst (Ctx.E, GA & " = addr " & To_String (V.Pir_Name));
            Emit_Inst (Ctx.E, LP & " = load.ptr " & GA & ", align 8");
            return LP;
         end;
      elsif V.Kind = VK_Local_Ptr
        and then (V.Owner_Subp = Ctx.Cur_Subp_Key or else V.Owner_Subp = 0 or else V.Captured_Slot <= 0)
      then
         declare
            LP : constant String := Next_Reg (Ctx.E, "lpl");
         begin
            Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (V.Pir_Name) & ", align 8");
            return LP;
         end;
      elsif V.Owner_Subp = Ctx.Cur_Subp_Key or else V.Owner_Subp = 0 or else V.Captured_Slot <= 0 then
         return To_String (V.Pir_Name);
      else
         -- Hoist & cache uplevel variable address load in .entry (collision-free 64-bit key)
         declare
            Cache_Key : constant Unsigned_64 :=
              Shift_Left (Unsigned_64 (V.Owner_Subp), 32) or Unsigned_64 (V.Captured_Slot);
            Found     : Boolean;
            F_Ptr, F_Ty : Unbounded_String;
         begin
            if Ctx.Uplevel_Cache.Contains (Cache_Key) then
               declare
                  Cached : constant String := To_String (Ctx.Uplevel_Cache.Element (Cache_Key));
               begin
                  if V.Kind = VK_Local_Ptr then
                     declare
                        LP : constant String := Next_Reg (Ctx.E, "ulpl");
                     begin
                        Emit_Inst (Ctx.E, LP & " = load.ptr " & Cached & ", align 8");
                        return LP;
                     end;
                  end if;
                  return Cached;
               end;
            end if;
            Walk_Static_Link (Ctx, V.Owner_Subp, Hoist_Entry => True, Found => Found, Frame_Ptr => F_Ptr, Frame_Type => F_Ty);
            if Found then
               declare
                  FP : constant String := Next_Reg (Ctx.E, "ufp");
                  VA : constant String := Next_Reg (Ctx.E, "uva");
               begin
                  Emit_Entry_Alloca (Ctx.E,
                    FP & " = field_ptr " & To_String (F_Ty) & ", " &
                    To_String (F_Ptr) & ", " & Img (V.Captured_Slot));
                  Emit_Entry_Alloca (Ctx.E, VA & " = load.ptr " & FP & ", align 8");
                  Ctx.Uplevel_Cache.Include (Cache_Key, To_Unbounded_String (VA));
                  if V.Kind = VK_Local_Ptr then
                     declare
                        LP : constant String := Next_Reg (Ctx.E, "ulpl");
                     begin
                        Emit_Inst (Ctx.E, LP & " = load.ptr " & VA & ", align 8");
                        return LP;
                     end;
                  end if;
                  return VA;
               end;
            end if;
         end;
         return To_String (V.Pir_Name);
      end if;
   end Emit_Var_Addr;

   function Emit_Captured_Slot_Ptr (Ctx : in out Context; Owner_Subp : Unsigned_32; Slot : Integer) return String is
      Found       : Boolean;
      F_Ptr, F_Ty : Unbounded_String;
   begin
      if Owner_Subp = Ctx.Cur_Subp_Key and then Length (Ctx.Cur_Frame_Reg) > 0
        and then Ctx.Subps.Contains (Owner_Subp)
      then
         declare
            SI : constant Subp_Info := Ctx.Subps.Element (Owner_Subp);
            FP : constant String := Next_Reg (Ctx.E, "csp");
         begin
            Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
              To_String (Ctx.Cur_Frame_Reg) & ", " & Img (Slot));
            return FP;
         end;
      end if;
      if Slot <= 0 then
         return "0";
      end if;
      Walk_Static_Link (Ctx, Owner_Subp, Hoist_Entry => True, Found => Found, Frame_Ptr => F_Ptr, Frame_Type => F_Ty);
      if Found then
         declare
            FP : constant String := Next_Reg (Ctx.E, "csp");
         begin
            Emit_Entry_Alloca (Ctx.E,
              FP & " = field_ptr " & To_String (F_Ty) & ", " &
              To_String (F_Ptr) & ", " & Img (Slot));
            return FP;
         end;
      end if;
      return "0";
   end Emit_Captured_Slot_Ptr;

   function Cleanup_Depth (Ctx : Context) return Natural is
   begin
      return Natural (Ctx.Cleanups.Length);
   end Cleanup_Depth;

   procedure Push_Cleanup (Ctx : in out Context; Kind : Cleanup_Kind; Reg : String) is
   begin
      Ctx.Cleanups.Append ((Kind => Kind, Reg => To_Unbounded_String (Reg)));
   end Push_Cleanup;

   procedure Pop_Cleanup (Ctx : in out Context; Expected : Cleanup_Kind) is
   begin
      if Ctx.Cleanups.Is_Empty or else Ctx.Cleanups.Last_Element.Kind /= Expected then
         raise Program_Error with "Cg_State.Pop_Cleanup: mismatched cleanup stack";
      end if;
      Ctx.Cleanups.Delete_Last;
   end Pop_Cleanup;

   function Latest_Cleanup_Reg (Ctx : Context; Kind : Cleanup_Kind) return Unbounded_String is
   begin
      if not Ctx.Cleanups.Is_Empty then
         for I in reverse 0 .. Natural (Ctx.Cleanups.Length) - 1 loop
            if Ctx.Cleanups.Element (I).Kind = Kind then
               return Ctx.Cleanups.Element (I).Reg;
            end if;
         end loop;
      end if;
      return Null_Unbounded_String;
   end Latest_Cleanup_Reg;

   function Needs_Unwind_To
     (Ctx           : Context;
      Target_Depth  : Natural;
      Leave_Masters : Boolean := True;
      Release_SS    : Boolean := True;
      Restore_SP    : Boolean := True) return Boolean
   is
   begin
      if Natural (Ctx.Cleanups.Length) <= Target_Depth then
         return False;
      end if;
      for I in Target_Depth .. Natural (Ctx.Cleanups.Length) - 1 loop
         case Ctx.Cleanups.Element (I).Kind is
            when CK_Master  => if Leave_Masters then return True; end if;
            when CK_SS_Mark => if Release_SS    then return True; end if;
            when CK_SP_Mark => if Restore_SP    then return True; end if;
         end case;
      end loop;
      return False;
   end Needs_Unwind_To;

   procedure Emit_Unwind_To
     (Ctx           : in out Context;
      Target_Depth  : Natural;
      Leave_Masters : Boolean := True;
      Release_SS    : Boolean := True;
      Restore_SP    : Boolean := True)
   is
      SS_Reg : Unbounded_String := Null_Unbounded_String;
      SP_Reg : Unbounded_String := Null_Unbounded_String;
      Dummy  : Unbounded_String;
   begin
      if Natural (Ctx.Cleanups.Length) <= Target_Depth then
         return;
      end if;
      -- Leave masters strictly innermost-to-outermost BEFORE releasing SS or restoring SP
      -- so dependent tasks completing in __ada_master_leave can still access enclosing storage.
      for I in reverse Target_Depth .. Natural (Ctx.Cleanups.Length) - 1 loop
         declare
            CE : constant Cleanup_Entry := Ctx.Cleanups.Element (I);
         begin
            case CE.Kind is
               when CK_Master =>
                  if Leave_Masters then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (CE.Reg), "", Dummy, Nounwind => True);
                  end if;
               when CK_SS_Mark =>
                  SS_Reg := CE.Reg;
               when CK_SP_Mark =>
                  SP_Reg := CE.Reg;
            end case;
         end;
      end loop;
      if Release_SS and then Length (SS_Reg) > 0 then
         Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (SS_Reg), "", Dummy, Nounwind => True);
      end if;
      if Restore_SP and then Length (SP_Reg) > 0 then
         Emit_Inst (Ctx.E, "stackrestore " & To_String (SP_Reg));
      end if;
   end Emit_Unwind_To;

end Cg_State;
