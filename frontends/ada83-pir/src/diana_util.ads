-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Interfaces; use Interfaces;
with USERPK; use USERPK;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;

package Diana_Util is

   -- Unique 32-bit key for any DIANA TREE pointer
   function Tree_Key (T : TREE) return Unsigned_32;
   function Tree_Key_Str (T : TREE) return String;

   -- Safe identifier/name extraction
   function Sym_Name (T : TREE) return String;
   function Lower_Name (S : String) return String;
   function Sanitize_Ident (S : String) return String;

   -- Canonicalize a subprogram / exception / type declaration node
   function Get_Name_Defn (Name_Node : TREE) return TREE;
   function Canonical_Def (Id : TREE) return TREE;
   function Canonical_Exc (Exc_Id : TREE) return TREE;
   function Resolve_Type_Spec (T : TREE) return TREE;
   function Base_Type_Spec (T : TREE) return TREE;

   -- Universal integer and real decoding from DIANA SM_VALUE nodes
   function Safe_SM_Value (Node : TREE) return TREE;
   function Has_Static_Val (T : TREE) return Boolean;
   function Eval_Int64 (Val_Node : TREE) return Integer_64;
   procedure Eval_Int64_Checked
     (Val_Node   : TREE;
      Val        : out Integer_64;
      Overflowed : out Boolean);
   function Eval_Float64 (Val_Node : TREE) return IEEE_Float_64;
   function Format_Float64 (V : IEEE_Float_64) return String;

   -- Type classification helpers
   function Is_Void_Tree (T : TREE) return Boolean;
   function Is_Boolean_Type (T : TREE) return Boolean;
   function Is_Character_Type (T : TREE) return Boolean;
   function Is_Discrete_Type (T : TREE) return Boolean;
   function Is_Integer_Type (T : TREE) return Boolean;
   function Is_Float_Type (T : TREE) return Boolean;
   function Is_Fixed_Type (T : TREE) return Boolean;
   function Is_Access_Type (T : TREE) return Boolean;
   function Is_Scalar_Type (T : TREE) return Boolean;
   function Is_Record_Type (T : TREE) return Boolean;
   function Is_Task_Type (T : TREE) return Boolean;
   function Is_Constrained_Array (T : TREE) return Boolean;
   function Is_Unconstrained_Array (T : TREE) return Boolean;
   function Is_Array_Type (T : TREE) return Boolean;
   function Is_String_Type (T : TREE) return Boolean;

   -- Scalar bit-width & PIR scalar type name
   function Scalar_Bits (T : TREE) return Natural;
   function Pir_Scalar_Type (T : TREE) return String;
   function Fixed_Small (T : TREE) return IEEE_Float_64;
   function Fixed_Attr_Small (T : TREE) return IEEE_Float_64;
   function Fixed_Delta (T : TREE) return IEEE_Float_64;

   -- Static range extraction for discrete subtypes (returns False if dynamic)
   procedure Get_Discrete_Bounds
     (T      : TREE;
      Static : out Boolean;
      First  : out Integer_64;
      Last   : out Integer_64);

   -- Check if a symbol belongs to a predefined RTL package (STANDARD, TEXT_IO, CALENDAR, etc.)
   function Enclosing_Package_Name (Id : TREE) return String;
   function Qualified_Ada_Name (Id : TREE) return String;
   function Is_Library_Level (Id : TREE) return Boolean;
   function Is_Predefined_Standard_Pkg (Pkg_Id : TREE) return Boolean;

   -- Convert a DIANA string literal node's symbol text into raw characters (unquoting "")
   function Decode_Ada_String_Lit (Raw : String) return String;

end Diana_Util;
