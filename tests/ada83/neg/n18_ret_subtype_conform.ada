-- Negative test: P3-7.13 return subtype mark mismatch between spec and body
procedure Neg_Ret_Subtype_Conform is
   subtype Pos_I is Integer range 1 .. 10;
   function F return Integer;
   function F return Pos_I is
   begin
      return 1;
   end F;
begin
   null;
end Neg_Ret_Subtype_Conform;
