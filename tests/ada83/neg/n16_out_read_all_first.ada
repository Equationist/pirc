-- Negative test: P3-7.13 OUT parameter read via P.all'First
procedure Neg_Out_Read_All_First is
   type Str_Acc is access String;
   procedure Bad (P : out Str_Acc; N : out Integer) is
   begin
      N := P.all'First;
      P := null;
   end Bad;
begin
   null;
end Neg_Out_Read_All_First;
