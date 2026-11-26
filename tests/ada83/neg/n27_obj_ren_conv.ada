-- Negative test: P3-7.16 illegal object renaming of type conversion & function call
procedure Neg_Obj_Ren_Conv is
   X : Integer := 1;
   R : Long_Integer renames Long_Integer (X);
begin
   null;
end Neg_Obj_Ren_Conv;
