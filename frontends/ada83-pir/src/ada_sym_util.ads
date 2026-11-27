-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

package Ada_Sym_Util is

   -- Deterministic 64-bit FNV-1a symbol cap (> 96 chars) shared by diana2pir and ada83_bind.
   -- Keeps the 48-char prefix + "_h" + 16-hex hash + optional Suffix.
   function Cap_Pir_Sym (Full : String; Suffix : String := "") return String;

   -- Lowercase + sanitize Ada identifier or operator symbol for PIR symbols.
   function Sanitize_Ada_Ident (S : String) return String;

   -- Canonical PIR entry symbol for a library-level parameterless main procedure.
   function Main_Entry_Sym (Unit_Name : String) return String;

end Ada_Sym_Util;
